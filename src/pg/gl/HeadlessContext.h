#pragma once
//
// An OpenGL 3.3 core context without a window or display, through EGL --
// what `pgshader render` and CI use. On Linux, Mesa provides it even without a
// GPU (llvmpipe). Built only when CMake finds EGL.
//
#include "pg/gl/Gl.h"

#include <string>

namespace pg::gl {

class HeadlessContext {
public:
    HeadlessContext() = default;
    ~HeadlessContext();
    HeadlessContext(const HeadlessContext&) = delete;
    HeadlessContext& operator=(const HeadlessContext&) = delete;

    /// Creates the context and makes it current. `error` says why not.
    bool create(std::string& error);
    /// eglGetProcAddress, in the shape Api::load wants.
    static Proc procAddress(const char* name);

private:
    void* display_ = nullptr;
    void* context_ = nullptr;
};

}  // namespace pg::gl
