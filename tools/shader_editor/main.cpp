// pgshadered -- the node-based shader editor.
//
//   pgshadered [GRAPH.pgsg] [--library FILE]... [--target NAME] [--mesh NAME]
//              [--size WxH] [--screenshot OUT.png [--frames N]]
//
// A window with the graph on the left and the live preview, uniforms and
// generated code on the right. --screenshot draws N frames (default 30),
// saves the window as a PNG and quits; it is how the editor is tested on a
// machine without a display (under xvfb-run).
#include "Editor.h"

#include "pg/gl/Png.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imnodes.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef PG_EXAMPLES_DIR
#define PG_EXAMPLES_DIR "examples/shaders"
#endif

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: pgshadered [GRAPH.pgsg] [--library FILE]... [--target NAME] [--mesh NAME]\n"
                 "                  [--size WxH] [--screenshot OUT.png [--frames N]]\n");
    return 2;
}

/// The first font file that exists, or empty.
std::string findFont(std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c;
    }
    return {};
}

void setupStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.ScaleAllSizes(scale);

    ImNodes::StyleColorsDark();
    ImNodesStyle& nodes = ImNodes::GetStyle();
    nodes.Flags |= ImNodesStyleFlags_GridLines;
    nodes.NodeCornerRounding = 5.0f;
    nodes.PinCircleRadius = 4.5f;
    nodes.LinkThickness = 3.0f;
    nodes.Colors[ImNodesCol_NodeBackground] = IM_COL32(44, 46, 52, 245);
    nodes.Colors[ImNodesCol_NodeBackgroundHovered] = IM_COL32(52, 54, 61, 245);
    nodes.Colors[ImNodesCol_NodeBackgroundSelected] = IM_COL32(58, 61, 70, 245);
    nodes.Colors[ImNodesCol_NodeOutline] = IM_COL32(90, 92, 100, 255);
    nodes.Colors[ImNodesCol_GridBackground] = IM_COL32(30, 31, 35, 255);
    nodes.Colors[ImNodesCol_GridLine] = IM_COL32(48, 50, 56, 255);
}

}  // namespace

int main(int argc, char** argv) {
    std::string graphPath, screenshot, target, mesh;
    std::vector<std::string> libraries;
    int frames = 30, width = 1600, height = 960;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--library") {
            const char* v = next();
            if (!v) return usage();
            libraries.push_back(v);
        } else if (a == "--screenshot") {
            const char* v = next();
            if (!v) return usage();
            screenshot = v;
        } else if (a == "--frames") {
            const char* v = next();
            if (!v) return usage();
            frames = std::max(1, std::atoi(v));
        } else if (a == "--size") {
            const char* v = next();
            if (!v || std::sscanf(v, "%dx%d", &width, &height) != 2) return usage();
            if (width < 320 || height < 240) return usage();
        } else if (a == "--target") {
            const char* v = next();
            if (!v) return usage();
            target = v;
        } else if (a == "--mesh") {
            const char* v = next();
            if (!v) return usage();
            mesh = v;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            return usage();
        } else if (graphPath.empty()) {
            graphPath = a;
        } else {
            return usage();
        }
    }

    glfwSetErrorCallback([](int code, const char* message) {
        std::fprintf(stderr, "glfw %d: %s\n", code, message);
    });
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);  // required on macOS
    GLFWwindow* window = glfwCreateWindow(width, height, "pgshadered", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    pg::gl::Api gl;
    std::string missing;
    if (!gl.load(glfwGetProcAddress, missing)) {
        std::fprintf(stderr, "pgshadered: OpenGL 3.3 functions missing: %s\n", missing.c_str());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImNodes::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the layout is fixed; nothing worth remembering
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    const float scale = std::max(1.0f, xscale);
    setupStyle(scale);

    // System fonts when there are any; Dear ImGui's built-in one otherwise.
    ImFont* codeFont = nullptr;
    const std::string sans = findFont({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                       "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                       "/System/Library/Fonts/Supplemental/Arial.ttf",
                                       "C:/Windows/Fonts/segoeui.ttf"});
    const std::string mono = findFont({"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                       "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                                       "/System/Library/Fonts/Menlo.ttc",
                                       "C:/Windows/Fonts/consola.ttf"});
    if (!sans.empty()) io.Fonts->AddFontFromFileTTF(sans.c_str(), 15.0f * scale);
    else io.Fonts->AddFontDefault();
    if (!mono.empty()) codeFont = io.Fonts->AddFontFromFileTTF(mono.c_str(), 14.0f * scale);

    ImNodes::GetIO().LinkDetachWithModifierClick.Modifier = &io.KeyCtrl;
    ImNodes::GetIO().EmulateThreeButtonMouse.Modifier = &io.KeyAlt;  // Alt + drag pans

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    int status = 0;
    {
        pg::editor::Editor editor(gl, libraries, PG_EXAMPLES_DIR);
        editor.setCodeFont(codeFont);
        if (!target.empty()) editor.setCodeTarget(target);
        if (!mesh.empty()) {
            bool found = false;
            for (pg::gl::MeshKind k : pg::gl::kMeshKinds) {
                if (mesh == pg::gl::meshName(k)) {
                    editor.setMesh(k);
                    found = true;
                }
            }
            if (!found) std::fprintf(stderr, "pgshadered: no mesh '%s'\n", mesh.c_str());
        }
        if (!graphPath.empty() && !editor.open(graphPath)) status = 1;

        int frame = 0;
        std::string title;
        while (!glfwWindowShouldClose(window) && !editor.quitRequested()) {
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            editor.frame(static_cast<float>(glfwGetTime()));
            ImGui::Render();
            if (editor.title() != title) {
                title = editor.title();
                glfwSetWindowTitle(window, title.c_str());
            }

            int w = 0, h = 0;
            glfwGetFramebufferSize(window, &w, &h);
            gl.BindFramebuffer(pg::gl::FRAMEBUFFER, 0);
            gl.Viewport(0, 0, w, h);
            gl.ClearColor(0.1f, 0.1f, 0.11f, 1.0f);
            gl.Clear(pg::gl::COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            if (!screenshot.empty() && ++frame >= frames) {
                std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
                gl.PixelStorei(pg::gl::PACK_ALIGNMENT, 1);
                gl.ReadPixels(0, 0, w, h, pg::gl::RGBA, pg::gl::UNSIGNED_BYTE, rgba.data());
                std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
                for (int y = 0; y < h; ++y) {  // GL rows go bottom up, PNG rows top down
                    const uint8_t* src = &rgba[static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 4];
                    uint8_t* dst = &rgb[static_cast<size_t>(y) * static_cast<size_t>(w) * 3];
                    for (int x = 0; x < w; ++x) std::memcpy(dst + x * 3, src + x * 4, 3);
                }
                if (!pg::gl::writePng(screenshot, w, h, 3, rgb)) {
                    std::fprintf(stderr, "pgshadered: cannot write %s\n", screenshot.c_str());
                    status = 1;
                }
                break;
            }
            glfwSwapBuffers(window);
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImNodes::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return status;
}
