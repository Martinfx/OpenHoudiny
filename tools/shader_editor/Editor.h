#pragma once
//
// The shader node editor: a client of pg::shader, like the pgshader CLI.
//
// Nothing here knows any particular node. The add-node menu, the pins and
// their colours, the widgets for values and params -- all of it is built from
// the NodeDefs of the library, so a node added to a .pgnodes file shows up
// here after Library > Reload, without recompiling the editor.
//
#include "pg/gl/Preview.h"
#include "pg/shader/Generator.h"

#include <map>
#include <string>
#include <vector>

struct ImFont;
struct ImVec2;

namespace pg::editor {

class Editor {
public:
    Editor(const gl::Api& gl, std::vector<std::string> libraryFiles, std::string examplesDir);

    bool open(const std::string& path);
    void newGraph();

    /// Draws the whole UI for one frame, rendering the preview on the way.
    void frame(float seconds);

    bool quitRequested() const { return quit_; }
    /// "marble.pgsg * - pgshadered": the file, and a star for unsaved changes.
    std::string title() const;

    /// The language the code panel shows: a TargetRegistry name.
    void setCodeTarget(const std::string& name);
    void setMesh(gl::MeshKind kind) { preview_.setMesh(kind); }
    /// A monospace font for the code panel; the default font when null.
    void setCodeFont(ImFont* font) { codeFont_ = font; }

private:
    // --- model -----------------------------------------------------------------
    void reloadLibrary();
    void recompile();
    bool save(const std::string& path);
    void exportShaders(const std::string& dir);
    void setStatus(std::string message, bool error = false);

    // --- UI --------------------------------------------------------------------
    void menuBar();
    void examplesMenu(const std::string& dir);
    void popups();
    void canvas();
    void nodeWidget(const shader::GraphNode& node, const shader::NodeDef* def);
    void handleCanvasEvents(bool canvasHovered);
    void frameAll(const ImVec2& canvasSize);
    void addNodePopup();
    void sidePanel(float seconds);
    void previewPanel(float seconds);
    void uniformsPanel();
    void codePanel();
    void problemsPanel();
    void statusBar();

    shader::NodeLibrary library_;
    std::vector<std::string> libraryFiles_;
    std::string examplesDir_;
    shader::ShaderGraph graph_;
    std::string path_;
    std::string savedText_;
    bool modified_ = false;  ///< the graph differs from the file; updated every frame

    gl::PreviewRenderer preview_;
    uint64_t compiledRevision_ = ~0ull;
    shader::GeneratedShader previewShader_;
    shader::GeneratedShader codeShader_;
    std::string codeTarget_ = "glsl330";
    std::string driverLog_;
    ImFont* codeFont_ = nullptr;
    bool pickFragmentTab_ = true;

    // Node positions to apply before the node is next drawn: grid space for
    // loaded graphs, screen space for nodes just added at the mouse.
    std::map<int, std::pair<float, float>> gridPositions_;
    std::map<int, std::pair<float, float>> screenPositions_;
    bool frameAll_ = true;  ///< pan so the graph is in view, after the next draw

    // add-node popup
    float addX_ = 0.0f, addY_ = 0.0f;
    int pendingPin_ = -1;  ///< a link dropped on empty canvas, to connect the new node to
    std::string search_;

    // path popups
    std::string pathInput_;
    const char* openPopup_ = nullptr;

    bool animate_ = true;
    float pausedTime_ = 0.0f;
    float sidePanelWidth_ = 520.0f;
    std::map<std::string, shader::Value> uniformValues_;

    std::string status_;
    bool statusIsError_ = false;
    bool quit_ = false;
};

}  // namespace pg::editor
