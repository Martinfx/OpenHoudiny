#include "pg/gl/HeadlessContext.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace pg::gl {
namespace {

std::string code(const char* what) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%04x", static_cast<unsigned>(eglGetError()));
    return std::string(what) + " (EGL error " + hex + ")";
}

/// The displays to try, in turn: Mesa's surfaceless platform -- no X
/// server, no GPU needed (llvmpipe runs on the CPU) -- then each GPU device
/// on its own (NVIDIA's way without a display), then the default display.
std::vector<std::pair<std::string, EGLDisplay>> candidates() {
    std::vector<std::pair<std::string, EGLDisplay>> out;
    auto getPlatformDisplay =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (getPlatformDisplay) {
        out.emplace_back("surfaceless", getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr));
#ifdef EGL_PLATFORM_DEVICE_EXT
        auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
        EGLDeviceEXT devices[8];
        EGLint count = 0;
        if (queryDevices && queryDevices(8, devices, &count)) {
            for (EGLint i = 0; i < count; ++i) {
                out.emplace_back("device " + std::to_string(i), getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr));
            }
        }
#endif
    }
    out.emplace_back("the default display", eglGetDisplay(EGL_DEFAULT_DISPLAY));
    return out;
}

/// An OpenGL 3.3 core context on `display`, current; null, with why, if not.
EGLContext contextOn(EGLDisplay display, std::string& why) {
    if (display == EGL_NO_DISPLAY) {
        why = "not there";
        return EGL_NO_CONTEXT;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        why = code("does not start");
        return EGL_NO_CONTEXT;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        why = code("cannot bind desktop OpenGL");
        eglTerminate(display);
        return EGL_NO_CONTEXT;
    }
    const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &count) || count == 0) {
        why = code("has no config for desktop OpenGL");
        eglTerminate(display);
        return EGL_NO_CONTEXT;
    }
    const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                                     EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        why = code("gives no OpenGL 3.3 core context");
        eglTerminate(display);
        return EGL_NO_CONTEXT;
    }
    // Rendering goes to our own framebuffers, so the context needs no surface.
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        why = code("cannot make the context current without a surface");
        eglDestroyContext(display, context);
        eglTerminate(display);
        return EGL_NO_CONTEXT;
    }
    return context;
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
    std::string tried;
    for (const auto& [name, display] : candidates()) {
        std::string why;
        EGLContext context = contextOn(display, why);
        if (context != EGL_NO_CONTEXT) {
            display_ = display;
            context_ = context;
            return true;
        }
        tried += (tried.empty() ? "" : "; ") + name + ": " + why;
    }
    error = "no EGL context without a window (" + tried + ")";
    return false;
}

Proc HeadlessContext::procAddress(const char* name) {
    return reinterpret_cast<Proc>(eglGetProcAddress(name));
}

}  // namespace pg::gl
