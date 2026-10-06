#pragma once
//
// The editor's Shaders network: a shader built from nodes (pg::shader). The
// graph on the canvas, the selected node's inputs and parameters, the live
// preview in the viewport, the generated code below it.
//
// Nothing here knows any particular node: the add menu, the pins and their
// colours, the parameter widgets are all built from the NodeDefs of the
// library, so a node added to a .pgnodes file shows up after Library >
// Reload without recompiling the editor.
//
#include "Commands.h"
#include "NodeCanvas.h"
#include "RenderJob.h"
#include "Thumbnails.h"
#include "Workspace.h"

#include "pg/gl/Preview.h"
#include "pg/shader/Generator.h"

#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pg::editor {

class ShaderWorkspace : public Workspace {
public:
    ShaderWorkspace(const gl::Api& gl, std::vector<std::string> libraryFiles, std::string examplesDir);

    const char* name() const override { return "Shaders"; }
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

    void newGraph();
    /// The language the code panel shows: a TargetRegistry name.
    void setCodeTarget(const std::string& name);
    void setMesh(gl::MeshKind kind) { preview_.setMesh(kind); }

protected:
    void saveThen(std::function<void()> then) override;

private:
    void reloadLibrary();
    void recompile();
    bool save(const std::string& path);
    void exportShaders(const std::string& dir);
    void savePreviewImage(const std::string& path);
    /// The preview animated -- five seconds of $time at 30 frames a second --
    /// into a video, behind the render modal.
    void savePreviewVideo(const std::string& path);
    void startValidation();
    void pollValidation();
    void setMessage(std::string message, bool error = false);
    void restore(const std::string& state);
    void examplesMenu(const std::string& dir);

    std::vector<CanvasNode> canvasNodes() const;
    std::vector<CanvasLink> canvasLinks() const;
    CanvasModel canvasModel();
    bool addMenu(ImVec2 at, const PinRef* pending);
    void nodeMenu(int node);
    void duplicate(const std::vector<int>& nodes);

    // --- thumbnails: each node's swatch -----------------------------------------------
    /// The node shows a swatch: thumbnails are on, its own is not hidden,
    /// and it has an output -- or is the output.
    bool showsThumbnail(const shader::GraphNode& n) const;
    /// The shader of `node`'s swatch: the graph up to it, its first output
    /// as the colour, drawn opaque -- the output node's, the graph's own.
    shader::GeneratedShader swatchShader(int node) const;
    /// Draws the swatches on screen that are out of date, a few a frame of
    /// the window, and forgets those of nodes gone.
    void updateSwatches();
    void clearSwatches();

    void nodeParameters(const shader::GraphNode& node, const shader::NodeDef& def);
    void codePanel();
    void problemsPanel();
    void uniformsPanel();

    shader::NodeLibrary library_;
    std::vector<std::string> libraryFiles_;
    std::string examplesDir_;
    shader::ShaderGraph graph_;
    std::string path_;
    std::string savedText_;
    History history_;

    gl::PreviewRenderer preview_;
    uint64_t compiledRevision_ = ~0ull;
    shader::GeneratedShader previewShader_;
    shader::GeneratedShader codeShader_;
    std::string codeTarget_ = "glsl330";
    std::string driverLog_;
    bool pickMesh_ = true;
    bool pickFragmentTab_ = true;
    std::map<std::string, shader::Value> uniformValues_;

    NodeCanvas canvas_;
    std::string search_;

    // Thumbnails: the swatches in the nodes.
    const gl::Api& gl_;
    bool thumbnails_ = true;                ///< View > Node Thumbnails
    std::set<int> thumbnailsHidden_;        ///< nodes whose own was hidden (their menu)
    std::unique_ptr<gl::PreviewRenderer> swatchRenderer_;  ///< draws them; made when one is first wanted
    std::unique_ptr<Thumbnails> swatches_;
    uint64_t swatchRevision_ = ~0ull;       ///< the graph's revision swatchKey_ is of
    uint64_t swatchKey_ = 0;                ///< what the graph is: its text, hashed
    std::map<int, uint64_t> swatchFailed_;  ///< swatches that did not compile, and the key they did not at

    std::future<cli::CheckReport> validation_;
    cli::CheckReport validationReport_;
    bool hasValidation_ = false;
    uint64_t validatedRevision_ = 0;

    bool animate_ = true;
    float time_ = 0.0f;
    int bottomTab_ = 0;

    ui::FileBrowser files_;
    enum class FileAction { None, Open, SaveAs, Export, Image, Video, Library } fileAction_ = FileAction::None;
    std::function<void()> afterSave_;  ///< what waits for the graph to be saved as (Workspace::saveThen)
    /// Where Ctrl+S saves: a file picked first where there is none yet.
    void saveAsDialog();
    RenderJob job_;

    std::string message_;
    bool messageError_ = false;
};

}  // namespace pg::editor
