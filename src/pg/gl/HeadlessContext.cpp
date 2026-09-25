#include "pg/gl/HeadlessContext.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace pg::gl {
namespace {

EGLDisplay openDisplay() {
    // Surfaceless first: no X server, no GPU device needed (Mesa llvmpipe
    // runs on the CPU). Then whatever the default display is.
    auto getPlatformDisplay =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (getPlatformDisplay) {
        EGLDisplay d = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (d != EGL_NO_DISPLAY && eglInitialize(d, nullptr, nullptr)) return d;
    }
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d != EGL_NO_DISPLAY && eglInitialize(d, nullptr, nullptr)) return d;
    return EGL_NO_DISPLAY;
}

}  // namespace

HeadlessContext::~HeadlessContext() {
    if (display_) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_) eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }
}

bool HeadlessContext::create(std::string& error) {
    EGLDisplay display = openDisplay();
    if (display == EGL_NO_DISPLAY) {
        error = "no EGL display";
        return false;
    }
    display_ = display;
    if (!eglBindAPI(EGL_OPENGL_API)) {
        error = "EGL cannot bind desktop OpenGL";
        return false;
    }
    const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                    EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &count) || count == 0) {
        error = "no EGL config for desktop OpenGL";
        return false;
    }
    const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                                     EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                     EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        error = "cannot create an OpenGL 3.3 core context";
        return false;
    }
    context_ = context;
    // Rendering goes to our own framebuffer, so the context needs no surface.
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        error = "cannot make the context current (no EGL_KHR_surfaceless_context?)";
        return false;
    }
    return true;
}

Proc HeadlessContext::procAddress(const char* name) {
    return reinterpret_cast<Proc>(eglGetProcAddress(name));
}

}  // namespace pg::gl
