#pragma once
//
// An OpenGL context to render pictures with, nothing shown: EGL without a
// window where the build has EGL (Mesa's surfaceless platform, a GPU device,
// the default display); else -- or when EGL gives none -- a hidden window of
// GLFW's, in the builds with the editor. What `prototype render` and
// `prototype sim` draw with.
//
#include "pg/gl/Gl.h"

#include <string>

#if defined(PG_HAVE_EGL) || defined(PG_HAVE_GUI)
#define PG_CAN_RENDER 1
#endif

namespace pg::cli {

class Offscreen {
public:
    Offscreen() = default;
    ~Offscreen();
    Offscreen(const Offscreen&) = delete;
    Offscreen& operator=(const Offscreen&) = delete;

    /// Makes a context current. False, with what each way said, if none.
    bool create(std::string& error);
    /// Where the context's functions are found, for gl::Api::load.
    gl::GetProc procAddress() const;
    /// "EGL", "a hidden window"
    const char* kind() const { return kind_; }

private:
    const char* kind_ = "";
    void* egl_ = nullptr;     // gl::HeadlessContext
    void* window_ = nullptr;  // GLFWwindow
    bool glfw_ = false;       // glfwInit succeeded
};

}  // namespace pg::cli
