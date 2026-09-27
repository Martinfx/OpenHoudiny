#include "Offscreen.h"

#ifdef PG_HAVE_EGL
#include "pg/gl/HeadlessContext.h"
#endif
#ifdef PG_HAVE_GUI
#include <GLFW/glfw3.h>
#endif

namespace pg::cli {
namespace {

#ifdef PG_HAVE_GUI
std::string glfwError;

void onGlfwError(int, const char* what) { glfwError = what ? what : "unknown"; }

gl::Proc glfwProc(const char* name) { return reinterpret_cast<gl::Proc>(glfwGetProcAddress(name)); }
#endif

}  // namespace

Offscreen::~Offscreen() {
#ifdef PG_HAVE_EGL
    delete static_cast<gl::HeadlessContext*>(egl_);
#endif
#ifdef PG_HAVE_GUI
    if (window_) glfwDestroyWindow(static_cast<GLFWwindow*>(window_));
    if (glfw_) glfwTerminate();
#endif
}

bool Offscreen::create(std::string& error) {
    std::string why;
#ifdef PG_HAVE_EGL
    {
        auto* context = new gl::HeadlessContext();
        std::string e;
        if (context->create(e)) {
            egl_ = context;
            kind_ = "EGL";
            return true;
        }
        delete context;
        why = "EGL: " + e;
    }
#endif
#ifdef PG_HAVE_GUI
    {
        // A window never shown: what the editor draws with, on a display.
        glfwSetErrorCallback(onGlfwError);
        glfwError.clear();
        if (glfwInit()) {
            glfw_ = true;
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
            GLFWwindow* w = glfwCreateWindow(64, 64, "pgshader", nullptr, nullptr);
            if (w) {
                glfwMakeContextCurrent(w);
                window_ = w;
                kind_ = "a hidden window";
                return true;
            }
        }
        why += std::string(why.empty() ? "" : "; ") + "a hidden window: " +
               (glfwError.empty() ? std::string("GLFW could not open one") : glfwError);
    }
#endif
    error = why.empty() ? std::string("this pgshader was built without EGL and without the editor: it draws no picture")
                        : "no OpenGL context to draw with -- " + why;
    return false;
}

gl::GetProc Offscreen::procAddress() const {
#ifdef PG_HAVE_EGL
    if (egl_) return gl::HeadlessContext::procAddress;
#endif
#ifdef PG_HAVE_GUI
    if (window_) return glfwProc;
#endif
    return nullptr;
}

}  // namespace pg::cli
