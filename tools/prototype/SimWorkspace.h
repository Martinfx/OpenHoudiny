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
// two a frame at a time behind a modal that shows how far it got (RenderJob)
// -- drawn as the viewport draws it, or rendered to the end by the Render
// tab's renderer, Cycles or the path tracer (FrameRender), on a thread of
// its own.
//
// It starts on an empty scene; File > Examples has finished ones.
//
// The displayed geometry is edited in the viewport as in Houdini
// (SimElements.cpp): 2, 3 and 4 pick its points, edges and primitives -- a
// click, a box, Shift adding, Ctrl taking away -- 1 the objects again. The
// handle of W, E and R moves, turns and sizes what is picked through an Edit
// node put after the displayed one; Ctrl+G makes a Group of it, Delete a
// Blast. P paints an attribute with a brush through an Attribute Paint node.
// Each is an ordinary node of the network, undone as any change is.
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
#include "FrameRender.h"
#include "Gizmo.h"
#include "NodeCanvas.h"
#include "RenderJob.h"
#include "RenderView.h"
#include "SimRunner.h"
#include "Thumbnails.h"
#include "Wedge.h"
#include "Workspace.h"

#include "pg/core/Pick.h"
#include "pg/core/Sculpt.h"
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
    void hidden() override { stopRender(); }
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
    /// The frames of the cache in `folder` in place of simulated ones, read
    /// as they are played (Simulation > Load Cache from Disk).
    bool openCache(const std::string& folder) { return loadCache(folder); }
    /// The most memory the frames take (Simulation > Cache Size).
    void setCacheSize(size_t bytes);
    /// What the viewport's gizmo does: select, move, rotate, scale.
    void setTool(GizmoMode tool) { tool_ = tool; }

    /// What a click in the viewport picks: objects -- nodes -- or the
    /// points, edges or primitives of the displayed geometry.
    enum class Elements { Objects, Points, Edges, Primitives };
    /// How a drag picks them: what a box holds, what a lasso drawn round
    /// goes round, what a brush goes over.
    enum class PickStyle { Box, Lasso, Brush };

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

    // --- thumbnails (SimThumbnails.cpp) ------------------------------------------------
    /// What a node's thumbnail shows, by what the node is; None: it has none.
    enum class ThumbKind { None, Geometry, Object, Source, Gas, Water, Pieces, Cloth, Rain, Camera, Output };
    static ThumbKind thumbKindOf(const sim::Node& n);
    /// The node shows a thumbnail: its kind has one, thumbnails are on, and
    /// its own is not hidden.
    bool showsThumbnail(const sim::Node& n) const;
    /// The geometry nodes whose thumbnails are on screen: what the cooker is
    /// asked for after what the viewport shows.
    std::vector<int> thumbnailGeometryWanted() const;
    /// A geometry node's geometry, as a cook made it: what its thumbnail
    /// shows, stamped anew when it changes.
    void noteThumbnailGeometry(int node, const GeometryPtr& geometry);
    /// Draws the thumbnails on screen that are out of date -- a few a frame
    /// of the window -- and forgets those of nodes gone.
    void updateThumbnails();
    /// Forgets every thumbnail: another network shows.
    void clearThumbnails();

    // --- the viewport -------------------------------------------------------------------
    /// The frame at the play head -- or, while the simulation has not got
    /// there yet, the latest before it -- if it is in memory: never waits
    /// for one read from disk (null meanwhile; it is read next).
    std::shared_ptr<const sim::Frame> frameToShow() const;
    /// Frame `n` if it is in memory, as frameToShow(); in a synchronous
    /// window -- screenshots, scripts -- read now, so that N frames of the
    /// window are N frames of the timeline.
    std::shared_ptr<const sim::Frame> readyFrame(int n) const;
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
    // The Render tab (SimRender.cpp): the path tracer on the shown scene.
    void renderTab(int width, int height);
    /// The camera a render sees through: the shot's at the frame on screen
    /// (`camera`, when there is one), else the view's -- `width` x `height`.
    sim::Camera renderCamera(int width, int height, bool camera) const;
    void stopRender();
    void saveRender(const std::string& path);
    /// Every frame of the shot -- 1 to the Output's last, as the simulation
    /// gets there -- into `target`: a video when its extension is one's,
    /// else a folder of numbered PNGs. `final`: rendered to the end by the
    /// Render tab's renderer, at its size; else drawn as the viewport draws.
    void startRender(const std::string& target, bool final = false);
    /// Frame `frame` of the shot for the render job, posed and drawn as the
    /// viewport would show it then. False while the simulation has not got
    /// there; false with why when it will not.
    bool drawShotFrame(int frame, std::vector<uint8_t>& rgb, std::string& error);
    /// The same rendered to the end by the Render tab's renderer, on a
    /// thread of its own (FrameRender): false while that renders it -- the
    /// job told how far it got -- or the simulation or the geometry has not
    /// got there.
    bool renderShotFrame(int frame, std::vector<uint8_t>& rgb, std::string& error);
    /// Frame `frame` of the simulation for the render job: null while the
    /// simulation is on its way there; null with why when it will not get
    /// there.
    std::shared_ptr<const sim::Frame> jobSimFrame(int frame, std::string& error);
    /// A frame the job rendered to the end, into the Render tab's texture:
    /// the tab and the job's modal show it.
    void showFinalFrame(const std::vector<uint8_t>& rgb);
    /// The Render tab's renderer, made at first need: Cycles' first
    /// pictures are of fewer, larger pixels anyway, so with it the tab
    /// renders the whole size from the start.
    RenderView& renderView();
    /// What the Render tab -- and a render to the end -- renders of the
    /// frame on screen at `width` x `height`: through the shot's camera
    /// (`camera`, when there is one), or the view.
    RenderView::Request renderRequest(int width, int height, bool camera);
    /// The size of a render to the end: the camera's picture, or the
    /// viewport's, at the Render tab's scale.
    void finalSize(int& width, int& height) const;
    /// The renderer a render to the end renders with, as the menus name it.
    std::string finalRenderer();
    /// Where a render goes by default: where the last one went, the
    /// network's folder, the current one if it can be written, or home.
    std::string renderFolder() const;
    /// A notice over the viewport: what was written, where, with Open and
    /// Show. An error -- or a `sticky` one, the end of a long render --
    /// stays until closed.
    void notify(std::string text, std::string path, bool error, bool sticky = false);
    void drawNotice(ImDrawList* d, ImVec2 lo, ImVec2 hi);
    /// The dialog of Render Video: the kinds of file there are to write.
    /// `final`: rendered to the end by the Render tab's renderer.
    void chooseVideo(bool final = false);
    void chooseFrames(bool final = false);
    /// A network with no node: what a new scene is.
    bool emptyScene() const { return net_.nodes().empty(); }
    /// A network that shows geometry and simulates nothing -- a model, a
    /// landscape an Output lights, an asset's inside: nothing to simulate is
    /// no problem.
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
    /// Where the time of the step to frame `f` went.
    void profilePanel(const sim::Frame& f);
    /// The wedge dialog: a parameter's values to bake a variant each.
    void wedgeDialog();
    /// The wedge's variants, how far each has got, and Show.
    void wedgePanel();
    /// Plays variant `i` of the wedge: its value into the parameter, its
    /// frames from its folder.
    void showVariant(size_t i);
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
    /// How fine the preview's grids are: "half as fine", "a quarter as fine".
    std::string previewFineness() const;
    /// What a status message adds when the network opened in the preview.
    std::string previewNote() const;
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
    void viewTools(ImVec2 at, float bottom);
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

    // --- editing the displayed geometry (SimElements.cpp) ------------------------------
    /// Picks another kind of element; what was picked becomes the same
    /// place in the new kind: the points of primitives, the edges between
    /// points...
    void setElements(Elements mode);
    /// Whether the viewport's left button picks elements or paints -- not
    /// objects.
    bool editingElements() const { return elements_ != Elements::Objects || paint_; }
    /// The displayed geometry, and what picks in it -- made again only when
    /// it moved; null when nothing is shown.
    const ElementPicker* picker();
    static PickView pickView(const ViewCamera& cam);
    /// The element under the mouse, of the kind picked; -1 for none.
    int32_t elementAt(const ViewCamera& cam, ImVec2 mouse);
    /// A click: that element alone, or added (Shift), or taken away (Ctrl).
    void clickElements(const ViewCamera& cam, ImVec2 mouse, bool add, bool remove);
    /// What a part of the screen takes in -- a box, a lasso, a brush's way
    /// -- the same way.
    void regionElements(const ViewCamera& cam, const ScreenRegion& region, bool add, bool remove);
    /// S: a box, a lasso, a brush; H: what the surface hides picked too.
    void setPickStyle(PickStyle style);
    void setPickHidden(bool on);
    /// The brush that picks: begun by the press -- a dab -- then over what
    /// it goes while the button is down; its ring.
    void pickBrushTool(ImDrawList* d, const ViewCamera& cam, bool overView, bool pressed);
    /// Its radius, pixels.
    float pickBrushRadius() const;

    // Soft selection (O): how far round what is picked a drag of the handle
    // takes the points along -- the Edit node's Soft Radius, Distance and
    // Falloff, shown on the geometry before and while it is dragged.
    struct Soft {
        bool on = false;
        float radius = 0.0f;  ///< m
        int metric = 0;       ///< 0 straight, 1 along the surface
        int falloff = 0;      ///< pg::Falloff
    };
    /// The Edit shown, when it is one of what is picked: the one a drag
    /// goes on with. 0 when not.
    int pickedEdit() const;
    /// Soft selection as it is: the shown Edit's of what is picked -- else
    /// the viewport's, what the next Edit is made with.
    Soft softNow() const;
    void setSoft(bool on);
    void setSoftRadius(float radius);
    void setSoftMetric(int metric);
    void setSoftFalloff(int falloff);
    /// The node whose geometry the shown Edit of what is picked moves -- its
    /// input: the shares are of that geometry. 0 for none.
    int softBaseNode() const;
    /// The share of a drag each point of the shown geometry takes; empty
    /// with soft selection off or nothing picked.
    const std::vector<float>& softShares();
    /// The ring of the radius round the handle's middle, and what it is.
    void drawSoftRing(ImDrawList* d, const ViewCamera& cam, const Vec3& center);
    void selectAllElements(bool invert);
    size_t elementCount() const;
    /// What is picked as the nodes read it: a pattern, and the class
    /// (0 points, 1 primitives) -- edges as their points.
    std::string elementPattern() const;
    int elementClass() const;
    /// The points what is picked moves: its points, the corners of its
    /// primitives, the ends of its edges.
    std::vector<uint8_t> elementPoints(const Geometry& geo) const;
    /// Their middle in the displayed geometry; false for none.
    bool elementCenter(Vec3& center) const;
    /// Forgets what was picked where the geometry it was of is no longer shown.
    void checkElements();
    /// The marks over the geometry: its wire, its points, what is picked,
    /// what is under the mouse, the paint.
    void updateOverlay();
    /// The surface's normal at each point of `geo` (not of unit length): a
    /// point's dot lies on it. Found again only for other points or faces.
    const std::vector<Vec3>& pointNormals(const GeometryPtr& geo);
    /// A node of `type` put after the displayed one -- fed by it, feeding
    /// what it fed -- and displayed. Its id; 0 when nothing is displayed.
    int insertAfterDisplayed(const std::string& type);
    /// Takes node `id` out, what fed it feeding what it fed again.
    void extractNode(int id);
    /// A Group of what is picked (Ctrl+G), a Blast of it (Delete).
    void groupElements();
    void deleteElements();
    /// The handle on what is picked, and the Edit node a drag of it sets.
    void elementGizmo(ImDrawList* d, const ViewCamera& cam, bool overView);
    void applyElementDrag(const GizmoDrag& drag);
    /// Escape during a drag: the Edit as it was -- gone, when the drag made it.
    void restoreElementDrag();
    /// The brush: P on and off -- on what is shown when it is an Attribute
    /// Paint or a Sculpt, else a new Attribute Paint; U: a Sculpt. Its
    /// strokes while the button is down.
    void setPaint(bool on);
    void setSculpt(bool on);
    void setBrush(bool on, const char* type);
    void paintTool(ImDrawList* d, const ViewCamera& cam, bool overView);
    /// The Attribute Paint node painted into: the displayed one, if it is one.
    int paintNode() const;
    /// The Sculpt node sculpted: the displayed one, if it is one.
    int sculptNode() const;
    /// Either: the node the brush makes its dabs in.
    int brushNode() const { return paintNode() ? paintNode() : sculptNode(); }
    bool sculpting() const { return paint_ && sculptNode() != 0; }
    /// The tool a sculpting dab is made with now: the node's, Shift smooths.
    SculptDab::Tool sculptTool() const;
    void scaleBrush(float factor);
    /// What the bottom of the viewport says in these modes; empty for none.
    std::string elementStatus() const;
    /// What is picked as a pattern of elements of class `cls`: the points
    /// of primitives picked, the primitives all of whose points are...
    std::string patternFor(AttrClass cls) const;
    /// Tab in the viewport: a geometry node put after the displayed one,
    /// on what is picked -- its Group the pattern, its class ours. True
    /// when one was chosen.
    bool pickedMenu();
    void applyToPicked(const std::string& type);
    /// The handle of a PolyExtrude shown: an arrow along its faces' normal
    /// that sets Distance. True when it is there.
    bool extrudeGizmo(ImDrawList* d, const ViewCamera& cam, bool overView);
    /// The numbers of the points or primitives seen (N).
    void drawNumbers(ImDrawList* d, const ViewCamera& cam);

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
        std::set<int> thumbnailsHidden;
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
    ui::PickList addList_;  ///< the network's add menu
    std::string nameEdit_;
    int nameEditNode_ = 0;
    bool nameActive_ = false;  ///< the name is being typed in

    const gl::Api& gl_;
    gl::VolumeRenderer renderer_;
    std::string rendererLog_;
    std::unique_ptr<SimRunner> runner_;
    /// The geometry, cooked on a thread of its own -- after runner_, whose
    /// frames it reads: it goes first.
    std::unique_ptr<sim::Cooker> cooker_;
    std::string cookKey_;          ///< what was last asked of it
    double cookAsked_ = 0.0;       ///< when (ImGui time)
    uint64_t cookSerial_ = 0;      ///< ... its serial number
    uint64_t cookedSerial_ = 0;    ///< the one of the geometry shown: cookSerial_ once it is that
    double cookMs_ = 0.0;          ///< how long the last cook took
    GeometryPtr sheetGeometry_;    ///< the spreadsheet's node's, as last cooked
    int sheetGeometryNode_ = 0;
    bool synchronous_ = false;

    // Thumbnails: the pictures in the nodes.
    bool thumbnails_ = true;              ///< View > Node Thumbnails
    std::set<int> thumbnailsHidden_;      ///< nodes whose own was hidden (their menu)
    std::unique_ptr<gl::VolumeRenderer> thumbRenderer_;  ///< draws them; made when one is first wanted
    std::string thumbRendererLog_;        ///< why it could not be made
    std::unique_ptr<Thumbnails> thumbs_;
    /// The geometry nodes' geometry as cooks last made it: a stamp that
    /// changes with it, and the network's revision when it last did -- a
    /// change of the network shows at once, one of the frame as it plays.
    struct ThumbGeometry {
        GeometryPtr geometry;
        uint64_t stamp = 0, revision = 0;
    };
    std::map<int, ThumbGeometry> thumbGeometry_;
    uint64_t thumbStamp_ = 0;
    std::string thumbCookKey_;            ///< what the cooker was last asked for them
    uint64_t thumbSerial_ = 0;            ///< ... that request's serial

    // Playback.
    int current_ = 1;
    bool playing_ = true, loop_ = true;
    double clock_ = 0.0;
    int lastCurrent_ = 1, direction_ = 1;  ///< which way the play head went: what is read ahead
    /// Big frames drawn coarser while they change -- playing, scrubbing --
    /// and as they are when the play head rests (View > Proxies).
    bool proxies_ = true;
    bool shownProxy_ = false;   ///< what is drawn is the coarser one
    double shownAt_ = 0.0;      ///< when the frame on screen was put there

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
    bool geometryFramed_ = false;  ///< the geometry shown framed since the network opened
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

    // Editing the displayed geometry.
    Elements elements_ = Elements::Objects;
    /// What is picked, of the geometry of the displayed node `node`: one
    /// byte a point or primitive, or the edges, sorted.
    struct Picked {
        int node = 0;
        size_t points = 0, primitives = 0;  ///< what that geometry had
        std::vector<uint8_t> mask;
        std::vector<Edge> edges;
        uint64_t revision = 0;              ///< changes with what is picked
    } picked_;
    Picked brushBefore_;                    ///< what was picked before the brush's stroke: Escape puts it back
    mutable std::string patternCache_;      ///< elementPattern(), for the revision and kind it was found for
    mutable uint64_t patternRevision_ = ~0ull;
    mutable Elements patternElements_ = Elements::Objects;
    std::unique_ptr<ElementPicker> picker_;
    int32_t hoverElement_ = -1;             ///< under the mouse, of the kind picked
    ImVec2 hoverMouse_{-1.0f, -1.0f};       ///< where the mouse was when it was found
    Vec3 hoverEye_, hoverForward_;          ///< ... and the camera
    const Geometry* hoverGeometry_ = nullptr;
    bool boxing_ = false;                   ///< a box or a lasso is being drawn
    PickStyle pickStyle_ = PickStyle::Box;
    bool pickHidden_ = false;               ///< what the surface hides is picked too, and shown faint
    float overlayHidden_ = 0.0f;            ///< ... as the renderer was told
    std::vector<Vec2> lasso_;               ///< the lasso drawn so far, on the screen
    bool brushing_ = false;                 ///< the button is down, the brush picking
    bool brushRemoves_ = false;             ///< ... taking away: Ctrl at the press
    ImVec2 brushFrom_;                      ///< where it was the frame before
    float pickBrush_ = 0.0f;                ///< its radius, unscaled pixels; 0: the default
    bool soft_ = false;                     ///< soft selection, for the next Edit
    float softRadius_ = 0.0f;               ///< m; 0: not set -- a share of the geometry's size
    int softMetric_ = 0, softFalloff_ = 0;
    GeometryPtr softBase_;                  ///< what the shown Edit of what is picked moves
    int softBaseNode_ = 0;                  ///< ... the node it is of
    std::vector<float> softShares_;         ///< softShares(), and what they were found for
    std::string softKey_;
    bool pressTurns_ = false;               ///< the press was Alt's or Space's: it turns the view
    bool pressCancelled_ = false;           ///< Escape during the press: it picks nothing
    GeometryPtr edgeGeometry_;              ///< the geometry of the wire drawn
    std::vector<Edge> edges_;               ///< ... its edges
    bool spaceUsed_ = false;                ///< Space held turned the view: letting go does not play
    std::string overlayKey_[gl::VolumeRenderer::kOverlayLayers];  ///< what the overlay's layers were made of
    GeometryPtr normalsGeometry_;           ///< the geometry of the normals the points are drawn over
    std::vector<Vec3> normals_;             ///< ... at each point, from the faces round it
    // A drag of the handle on elements: the Edit node, its values when it
    // began, the handle's middle then; made by this drag, it goes on Escape.
    int editNode_ = 0;
    bool editMade_ = false;
    bool editPending_ = false;              ///< a drag began: its Edit is made once it moves something
    std::string editPattern_;               ///< ... of what was picked then
    int editClass_ = 0;
    /// The network before a node was put in for a drag or the brush, and
    /// just after: taken out again untouched, it is as it was before.
    std::string madeBefore_, madeAfter_;
    Vec3 editT0_, editR0_, editS0_{1.0f, 1.0f, 1.0f}, editP0_, editCenter0_;
    // The handle of a PolyExtrude: its node, Distance when the drag began,
    // the faces' middle and normal then.
    int extrudeNode_ = 0;
    float extrudeDistance0_ = 0.0f;
    Vec3 extrudeCenter0_, extrudeNormal0_;
    // The numbers shown (N): where each is, found again as the view or the
    // geometry changes.
    bool numbers_ = false;
    std::vector<std::pair<Vec3, uint32_t>> numberAt_;
    std::string numbersKey_;
    std::string pickedSearch_;  ///< what Tab's menu is searched for
    ui::PickList pickedList_;
    // The brush.
    bool paint_ = false;
    bool stroking_ = false;                 ///< the button is down, painting
    bool strokeOn_ = false;                 ///< ... and the brush on the surface since the last dab
    bool paintMade_ = false;                ///< its node made by P, unpainted yet
    int paintNode_ = 0;
    Vec3 lastDab_;
    bool brushHit_ = false;                 ///< the brush is on the surface
    Vec3 brushAt_, brushNormal_;
    bool grabbing_ = false;                 ///< sculpting's Grab: the dab the mouse moves
    SculptDab grab_;
    std::string grabBefore_;                ///< ... the strokes before it
    Vec3 addAt_;                   ///< where the add menu puts what it adds
    int newColor_ = 0;

    ui::FileBrowser files_;
    enum class FileAction {
        None, Open, SaveAs, Image, Frames, Video, FinalFrames, FinalVideo, MeshFile, ImportMesh, SaveCache, LoadCache, Bake,
        ExportGeometry, ExportFrames, ExportUsd, OpenAsset, SaveAsset, SaveRender
    } fileAction_ = FileAction::None;
    int fileNode_ = 0;        ///< MeshFile: the node whose file is chosen; Export...: whose geometry
    std::string fileParam_;
    std::string cacheFolder_;  ///< the folder the cache was last saved to or loaded from

    // Preview and bakes.
    bool preview_ = false;     ///< the gas and the water simulated on coarser grids (sim::preview)
    bool forcedPreview_ = false;  ///< preview_ set by the network opened (its Output's Open in Preview)
    Bake bake_;
    std::string bakeFolder_;   ///< where the last bake went
    double bakePolled_ = 0.0;  ///< when the bake and the frames on disk were last looked at (ImGui time)
    Wedge wedge_;
    bool wedgeOpen_ = false;   ///< the wedge dialog is to open
    int wedgeNode_ = 0;        ///< ... for this node's
    std::string wedgeParam_;   ///< ... parameter
    float wedgeFrom_ = 0.0f, wedgeTo_ = 1.0f;
    int wedgeCount_ = 4;
    std::string wedgeFolder_;
    std::string wedgeError_;
    int wedgeShown_ = -1;      ///< the variant played; -1: none

    // Rendering.
    RenderJob job_;
    int jobWidth_ = 0, jobHeight_ = 0;  ///< the size of the job's frames, fixed when it starts
    int jobReturnFrame_ = 1;            ///< the play head, put back when the job ends
    bool jobWasPlaying_ = false;
    // A job rendered to the end: by what, the frame it renders, whether the
    // Render tab was going on (paused while the job renders), whether its
    // texture holds the job's last frame.
    bool jobFinal_ = false;
    RenderView::Engine jobEngine_ = RenderView::Engine::PathTracer;
    std::unique_ptr<FrameRender> frameRender_;
    int jobFrame_ = 0;
    bool jobPausedView_ = false;
    bool jobShown_ = false;
    std::string renderFolder_;          ///< where the last render went
    // The Render tab.
    bool renderTabOn_ = false;
    bool renderAutoPaused_ = false;  ///< paused as the tab was left: goes on when it is back
    std::unique_ptr<RenderView> renderView_;
    gl::GLuint renderTexture_ = 0;
    int renderTextureW_ = 0, renderTextureH_ = 0;
    int renderScale_ = 1;               ///< into 25, 50, 100 %
    uint64_t renderKey_ = 0;
    render::Settings renderSettings_;
    /// The plate the Render tab's CG goes over: the shot camera's, of the
    /// frame shown -- read once, kept while the file and the camera stay.
    std::shared_ptr<const render::Plate> renderPlate_;
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
