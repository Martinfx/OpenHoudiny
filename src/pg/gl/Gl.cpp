#include "pg/gl/Gl.h"

#include <algorithm>

namespace pg::gl {
namespace {

GLuint compile(const Api& gl, GLenum stage, const std::string& source, std::string& log) {
    const GLuint s = gl.CreateShader(stage);
    const GLchar* text = source.c_str();
    gl.ShaderSource(s, 1, &text, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        gl.GetShaderiv(s, INFO_LOG_LENGTH, &len);
        std::string msg(static_cast<size_t>(std::max(len, 1)), '\0');
        gl.GetShaderInfoLog(s, len, nullptr, msg.data());
        log += (stage == VERTEX_SHADER ? "vertex: " : "fragment: ") + std::string(msg.c_str());
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

bool Api::load(GetProc getProc, std::string& missing) {
#define PG_GL_LOAD(name, ret, params)                                         \
    name = reinterpret_cast<ret(PG_GLAPI*) params>(getProc("gl" #name));      \
    if (!name) {                                                              \
        missing = "gl" #name;                                                 \
        return false;                                                         \
    }
    PG_GL_FUNCTIONS(PG_GL_LOAD)
#undef PG_GL_LOAD
    return true;
}

GLuint buildProgram(const Api& gl, const std::string& vertex, const std::string& fragment, std::string& log) {
    log.clear();
    const GLuint vs = compile(gl, VERTEX_SHADER, vertex, log);
    const GLuint fs = compile(gl, FRAGMENT_SHADER, fragment, log);
    if (!vs || !fs) {
        if (vs) gl.DeleteShader(vs);
        if (fs) gl.DeleteShader(fs);
        return 0;
    }
    const GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs);
    gl.AttachShader(p, fs);
    gl.LinkProgram(p);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(p, LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        gl.GetProgramiv(p, INFO_LOG_LENGTH, &len);
        std::string msg(static_cast<size_t>(std::max(len, 1)), '\0');
        gl.GetProgramInfoLog(p, len, nullptr, msg.data());
        log = "link: " + std::string(msg.c_str());
        gl.DeleteProgram(p);
        return 0;
    }
    return p;
}

std::vector<uint8_t> readRgb(const Api& gl, GLuint fbo, int width, int height, int factor) {
    factor = std::max(factor, 1);
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    gl.BindFramebuffer(FRAMEBUFFER, fbo);
    gl.PixelStorei(PACK_ALIGNMENT, 1);
    gl.ReadPixels(0, 0, width, height, RGBA, UNSIGNED_BYTE, rgba.data());
    gl.BindFramebuffer(FRAMEBUFFER, 0);

    // GL rows run bottom to top; images top to bottom. Average factor x factor.
    const int w = width / factor, h = height / factor;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                int sum = 0;
                for (int dy = 0; dy < factor; ++dy) {
                    for (int dx = 0; dx < factor; ++dx) {
                        const int sy = height - 1 - (y * factor + dy), sx = x * factor + dx;
                        sum += rgba[(static_cast<size_t>(sy) * static_cast<size_t>(width) + static_cast<size_t>(sx)) * 4 +
                                    static_cast<size_t>(c)];
                    }
                }
                rgb[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3 + static_cast<size_t>(c)] =
                    static_cast<uint8_t>(sum / (factor * factor));
            }
        }
    }
    return rgb;
}

}  // namespace pg::gl
