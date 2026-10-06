// The editor's window -- what `prototype` opens when it is given no command:
//
//   prototype [NETWORK.pgsim | GRAPH.pgsg] [--example NAME] [--shaders] [--select NODE]
//            [--library FILE]... [--target NAME] [--mesh NAME] [--cache DIR] [--cache-size MB]
//            [--size WxH] [--screenshot OUT.png [--frames N]] [--script FILE] [--recovery DIR]
//
// It opens on the Simulation network, with the campfire example -- or the
// file given, in the network it belongs to. --cache plays the frames of a
// cache on disk in place of simulating them, read as they are played;
// --cache-size the most memory the frames take (Simulation > Cache Size),
// past it they go to disk. --screenshot draws N frames
// (default 30), saves the window as a PNG and quits: how the editor is tested
// on a machine without a display (under xvfb-run). Then each frame of the
// window is one step of the simulation, so N frames show frame N.
//
// --recovery: where what is not saved is kept as it is worked on, to be
// recovered after a crash (Recovery.h) -- by default the user's state
// folder; a screenshot keeps nothing.
//
// --script plays input from a file into the window, a step a frame, and
// quits at its end: the mouse, the keys, screenshots along the way. How the
// editor's interactions are tested without a person at it:
//
//   move X Y            the mouse to a point of the window
//   click X Y [right|middle]      dclick X Y      a (double) click there
//   drag X1 Y1 X2 Y2 [right|middle] [STEPS]       press, move, let go
//   down / up [left|right|middle]                 wheel D
//   key [ctrl+][shift+]NAME      tab, enter, escape, delete, space, left,
//                                right, home, end, backspace, f5, a..z, 0..9, [ ]
//   hold / release ctrl|shift|alt                 a modifier down over the steps between
//   type TEXT           characters, as typed
//   wait N              N frames with no input
//   shot FILE           the window as a PNG, now
#include "App.h"

#include "Commands.h"
#include "Editor.h"
#include "Theme.h"

#include "pg/gl/Png.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// The imgui.h on the include path has to be the one whose imgui.cpp is built
// in. Another copy -- a system package next to other headers -- compiles and
// then crashes; CMake passes the version it fetched.
#ifdef PG_IMGUI_VERSION_NUM
static_assert(IMGUI_VERSION_NUM == PG_IMGUI_VERSION_NUM,
              "imgui.h is not the Dear ImGui fetched for this build: another copy on the include path "
              "shadows it");
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef PG_EXAMPLES_DIR
#define PG_EXAMPLES_DIR "examples/shaders"
#endif

namespace {

int usage() {
    pg::cli::printUsage(stderr);
    return 2;
}

/// Input played into the window from a file (--script).
class Script {
public:
    bool load(const std::string& path, std::string& error) {
        std::ifstream in(path);
        if (!in) {
            error = path + ": cannot read it";
            return false;
        }
        std::string line;
        int number = 0;
        while (std::getline(in, line)) {
            ++number;
            if (const size_t hash = line.find('#'); hash != std::string::npos && line.rfind("type", 0) != 0) {
                line = line.substr(0, hash);
            }
            std::istringstream words(line);
            std::string cmd;
            if (!(words >> cmd)) continue;
            auto fail = [&](const std::string& why) {
                error = path + ":" + std::to_string(number) + ": " + why;
                return false;
            };
            auto buttonOf = [](const std::string& b) { return b == "right" ? 1 : b == "middle" ? 2 : 0; };
            if (cmd == "move" || cmd == "click" || cmd == "dclick") {
                float x = 0, y = 0;
                std::string b;
                if (!(words >> x >> y)) return fail(cmd + " X Y");
                words >> b;
                steps_.push_back({Step::Move, x, y});
                if (cmd == "move") continue;
                for (int i = 0; i < (cmd == "dclick" ? 2 : 1); ++i) {
                    steps_.push_back({Step::Down, 0, 0, buttonOf(b)});
                    steps_.push_back({Step::Up, 0, 0, buttonOf(b)});
                }
            } else if (cmd == "drag") {
                float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                if (!(words >> x0 >> y0 >> x1 >> y1)) return fail("drag X1 Y1 X2 Y2");
                std::string b;
                int n = 12;
                words >> b;
                if (!b.empty() && std::isdigit(static_cast<unsigned char>(b[0]))) {
                    n = std::stoi(b);
                    b.clear();
                } else {
                    words >> n;
                }
                steps_.push_back({Step::Move, x0, y0});
                steps_.push_back({Step::Down, 0, 0, buttonOf(b)});
                for (int i = 1; i <= std::max(1, n); ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(std::max(1, n));
                    steps_.push_back({Step::Move, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t});
                }
                steps_.push_back({Step::Up, 0, 0, buttonOf(b)});
            } else if (cmd == "down" || cmd == "up") {
                std::string b;
                words >> b;
                steps_.push_back({cmd == "down" ? Step::Down : Step::Up, 0, 0, buttonOf(b)});
            } else if (cmd == "wheel") {
                float d = 0;
                if (!(words >> d)) return fail("wheel D");
                steps_.push_back({Step::Wheel, d});
            } else if (cmd == "key") {
                std::string k;
                if (!(words >> k)) return fail("key NAME");
                Step s{Step::Key};
                for (;;) {
                    if (k.rfind("ctrl+", 0) == 0) s.mods |= ImGuiMod_Ctrl, k = k.substr(5);
                    else if (k.rfind("shift+", 0) == 0) s.mods |= ImGuiMod_Shift, k = k.substr(6);
                    else if (k.rfind("alt+", 0) == 0) s.mods |= ImGuiMod_Alt, k = k.substr(4);
                    else break;
                }
                s.key = keyOf(k);
                if (s.key == ImGuiKey_None) return fail("no key '" + k + "'");
                steps_.push_back(s);
                steps_.push_back({Step::KeyUp, 0, 0, 0, s.key, s.mods});
            } else if (cmd == "type") {
                std::string rest;
                std::getline(words, rest);
                if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
                steps_.push_back({Step::Type, 0, 0, 0, ImGuiKey_None, 0, rest});
            } else if (cmd == "hold" || cmd == "release") {
                // A modifier held down over the steps between: hold shift ... release shift
                std::string k;
                if (!(words >> k)) return fail(cmd + " ctrl|shift|alt");
                const ImGuiKey mod = k == "ctrl" ? ImGuiMod_Ctrl : k == "shift" ? ImGuiMod_Shift : k == "alt" ? ImGuiMod_Alt : ImGuiKey_None;
                if (mod == ImGuiKey_None) return fail("hold and release take ctrl, shift or alt");
                steps_.push_back({cmd == "hold" ? Step::Hold : Step::Release, 0, 0, 0, mod});
            } else if (cmd == "wait") {
                int n = 1;
                words >> n;
                for (int i = 0; i < n; ++i) steps_.push_back({Step::Wait});
            } else if (cmd == "shot") {
                std::string file;
                if (!(words >> file)) return fail("shot FILE");
                steps_.push_back({Step::Shot, 0, 0, 0, ImGuiKey_None, 0, file});
            } else {
                return fail("unknown command '" + cmd + "'");
            }
        }
        return true;
    }

    bool done() const { return steps_.empty(); }

    /// Feeds this frame's step. A screenshot to take after the frame goes
    /// into `shot`.
    void feed(ImGuiIO& io, std::string& shot) {
        if (steps_.empty()) return;
        const Step s = steps_.front();
        steps_.pop_front();
        switch (s.kind) {
            case Step::Move: io.AddMousePosEvent(s.x, s.y); break;
            case Step::Down: io.AddMouseButtonEvent(s.button, true); break;
            case Step::Up: io.AddMouseButtonEvent(s.button, false); break;
            case Step::Wheel: io.AddMouseWheelEvent(0.0f, s.x); break;
            case Step::Key:
                if (s.mods & ImGuiMod_Ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
                if (s.mods & ImGuiMod_Shift) io.AddKeyEvent(ImGuiMod_Shift, true);
                if (s.mods & ImGuiMod_Alt) io.AddKeyEvent(ImGuiMod_Alt, true);
                io.AddKeyEvent(s.key, true);
                break;
            case Step::KeyUp:
                io.AddKeyEvent(s.key, false);
                if (s.mods & ImGuiMod_Ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
                if (s.mods & ImGuiMod_Shift) io.AddKeyEvent(ImGuiMod_Shift, false);
                if (s.mods & ImGuiMod_Alt) io.AddKeyEvent(ImGuiMod_Alt, false);
                break;
            case Step::Hold: io.AddKeyEvent(s.key, true); break;
            case Step::Release: io.AddKeyEvent(s.key, false); break;
            case Step::Type: io.AddInputCharactersUTF8(s.text.c_str()); break;
            case Step::Wait: break;
            case Step::Shot: shot = s.text; break;
        }
    }

private:
    struct Step {
        enum Kind { Move, Down, Up, Wheel, Key, KeyUp, Hold, Release, Type, Wait, Shot } kind;
        float x = 0.0f, y = 0.0f;
        int button = 0;
        ImGuiKey key = ImGuiKey_None;
        int mods = 0;
        std::string text;

        Step(Kind k, float x_ = 0.0f, float y_ = 0.0f, int b = 0, ImGuiKey k2 = ImGuiKey_None, int m = 0,
             std::string t = {})
            : kind(k), x(x_), y(y_), button(b), key(k2), mods(m), text(std::move(t)) {}
    };

    static ImGuiKey keyOf(const std::string& k) {
        if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z') return static_cast<ImGuiKey>(ImGuiKey_A + (k[0] - 'a'));
        if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (k[0] - '0'));
        static const std::pair<const char*, ImGuiKey> names[] = {
            {"tab", ImGuiKey_Tab},       {"enter", ImGuiKey_Enter},   {"escape", ImGuiKey_Escape},
            {"delete", ImGuiKey_Delete}, {"space", ImGuiKey_Space},   {"left", ImGuiKey_LeftArrow},
            {"right", ImGuiKey_RightArrow}, {"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow},
            {"home", ImGuiKey_Home},     {"end", ImGuiKey_End},       {"backspace", ImGuiKey_Backspace},
            {"f5", ImGuiKey_F5},         {"f12", ImGuiKey_F12},       {"[", ImGuiKey_LeftBracket},
            {"]", ImGuiKey_RightBracket}};
        for (const auto& [name, key] : names) {
            if (k == name) return key;
        }
        return ImGuiKey_None;
    }

    std::deque<Step> steps_;
};

bool saveWindow(const pg::gl::Api& gl, int w, int h, const std::string& path) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    gl.PixelStorei(pg::gl::PACK_ALIGNMENT, 1);
    gl.ReadPixels(0, 0, w, h, pg::gl::RGBA, pg::gl::UNSIGNED_BYTE, rgba.data());
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {  // GL rows go bottom up, PNG rows top down
        const uint8_t* src = &rgba[static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 4];
        uint8_t* dst = &rgb[static_cast<size_t>(y) * static_cast<size_t>(w) * 3];
        for (int x = 0; x < w; ++x) std::memcpy(dst + x * 3, src + x * 4, 3);
    }
    return pg::gl::writePng(path, w, h, 3, rgb);
}

}  // namespace

namespace pg::editor {

int runEditor(int argc, char** argv) {
    std::string path, screenshot, target, mesh, example, select, scriptPath, cache, recovery;
    bool recoveryGiven = false;
    std::vector<std::string> libraries;
    bool shaders = false;
    int frames = 30, width = 1600, height = 960, cacheMb = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--library") {
            if (!(v = next())) return usage();
            libraries.push_back(v);
        } else if (a == "--screenshot") {
            if (!(v = next())) return usage();
            screenshot = v;
        } else if (a == "--frames") {
            if (!(v = next())) return usage();
            frames = std::max(1, std::atoi(v));
        } else if (a == "--size") {
            if (!(v = next()) || std::sscanf(v, "%dx%d", &width, &height) != 2) return usage();
            if (width < 320 || height < 240) return usage();
        } else if (a == "--target") {
            if (!(v = next())) return usage();
            target = v;
        } else if (a == "--mesh") {
            if (!(v = next())) return usage();
            mesh = v;
        } else if (a == "--example") {
            if (!(v = next())) return usage();
            example = v;
        } else if (a == "--select") {
            if (!(v = next())) return usage();
            select = v;
        } else if (a == "--script") {
            if (!(v = next())) return usage();
            scriptPath = v;
        } else if (a == "--cache") {
            if (!(v = next())) return usage();
            cache = v;
        } else if (a == "--cache-size") {
            if (!(v = next()) || (cacheMb = std::atoi(v)) < 16) return usage();
        } else if (a == "--recovery") {
            if (!(v = next())) return usage();
            recovery = v;
            recoveryGiven = true;
        } else if (a == "--shaders") {
            shaders = true;
        } else if (!a.empty() && a[0] == '-') {
            return usage();
        } else if (path.empty()) {
            path = a;
        } else {
            return usage();
        }
    }

    Script script;
    if (!scriptPath.empty()) {
        std::string error;
        if (!script.load(scriptPath, error)) {
            std::fprintf(stderr, "prototype: %s\n", error.c_str());
            return 2;
        }
    }
    const bool scripted = !scriptPath.empty();

    glfwSetErrorCallback([](int code, const char* message) { std::fprintf(stderr, "glfw %d: %s\n", code, message); });
    if (!glfwInit()) {
        std::fprintf(stderr, "prototype: cannot open a window -- is there a display? The commands work without one:\n\n");
        pg::cli::printUsage(stderr);
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);  // required on macOS
    GLFWwindow* window = glfwCreateWindow(width, height, "Prototype", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "prototype: cannot create a window with OpenGL 3.3\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    pg::gl::Api gl;
    std::string missing;
    if (!gl.load(glfwGetProcAddress, missing)) {
        std::fprintf(stderr, "prototype: OpenGL 3.3 functions missing: %s\n", missing.c_str());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the layout is the editor's; nothing worth remembering
    // Played input: every event of a frame takes effect in it.
    if (scripted) io.ConfigInputTrickleEventQueue = false;
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    theme::apply(std::max(1.0f, xscale));

    const bool platform = ImGui_ImplGlfw_InitForOpenGL(window, true);
    if (!platform || !ImGui_ImplOpenGL3_Init("#version 330 core")) {
        std::fprintf(stderr, "prototype: Dear ImGui's %s backend did not start\n", platform ? "OpenGL 3" : "GLFW");
        if (platform) ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    int status = 0;
    {
        // A screenshot of frame N wants N steps: the simulation in step with
        // the window. A script runs it as a person would see it, on its thread.
        Editor editor(gl, libraries, PG_EXAMPLES_DIR, !screenshot.empty());
        ShaderWorkspace& sh = editor.shaders();
        if (!target.empty()) sh.setCodeTarget(target);
        if (!mesh.empty()) {
            bool found = false;
            for (pg::gl::MeshKind k : pg::gl::kMeshKinds) {
                if (mesh == pg::gl::meshName(k)) {
                    sh.setMesh(k);
                    found = true;
                }
            }
            if (!found) std::fprintf(stderr, "prototype: no mesh '%s'\n", mesh.c_str());
        }
        if (!example.empty() && !editor.openExample(example)) {
            std::fprintf(stderr, "prototype: no example '%s'\n", example.c_str());
            status = 1;
        }
        if (!path.empty() && !editor.open(path)) {
            std::fprintf(stderr, "prototype: cannot open %s\n", path.c_str());
            status = 1;
        }
        if (shaders) editor.showShaders();
        if (!select.empty()) editor.simulation().selectNode(select);
        if (cacheMb > 0) editor.simulation().setCacheSize(static_cast<size_t>(cacheMb) << 20);
        if (!cache.empty() && !editor.simulation().openCache(cache)) {
            std::fprintf(stderr, "prototype: %s\n", editor.simulation().message().c_str());
            status = 1;
        }
        // Autosaves, and those an editor that did not close left: not for a screenshot.
        editor.setRecovery(recoveryGiven ? recovery : screenshot.empty() ? defaultRecoveryFolder() : std::string());

        int frame = 0;
        std::string title;
        auto last = std::chrono::steady_clock::now();
        while (!editor.quitRequested()) {
            glfwPollEvents();
            // The window closed: as Quit -- changes not saved asked about first.
            if (glfwWindowShouldClose(window)) {
                glfwSetWindowShouldClose(window, GLFW_FALSE);
                editor.requestQuit();
            }
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(0.25f, std::chrono::duration<float>(now - last).count());
            last = now;
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            std::string shot;
            if (scripted) script.feed(io, shot);
            ImGui::NewFrame();
            editor.frame(dt);
            ImGui::Render();
            if (editor.title() != title) {
                title = editor.title();
                glfwSetWindowTitle(window, title.c_str());
            }

            int w = 0, h = 0;
            glfwGetFramebufferSize(window, &w, &h);
            gl.BindFramebuffer(pg::gl::FRAMEBUFFER, 0);
            gl.Viewport(0, 0, w, h);
            gl.ClearColor(0.09f, 0.09f, 0.1f, 1.0f);
            gl.Clear(pg::gl::COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            if (!shot.empty() && !saveWindow(gl, w, h, shot)) {
                std::fprintf(stderr, "prototype: cannot write %s\n", shot.c_str());
                status = 1;
            }
            if (scripted && script.done()) break;
            if (!screenshot.empty() && ++frame >= frames) {
                if (!saveWindow(gl, w, h, screenshot)) {
                    std::fprintf(stderr, "prototype: cannot write %s\n", screenshot.c_str());
                    status = 1;
                }
                break;
            }
            glfwSwapBuffers(window);
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return status;
}

}  // namespace pg::editor
