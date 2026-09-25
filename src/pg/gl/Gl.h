#pragma once
//
// The slice of OpenGL 3.3 core the preview renderer uses, loaded through a
// function the caller provides (glfwGetProcAddress, eglGetProcAddress, ...).
//
// No GL headers and no loader library: the types, constants and entry points
// needed are declared here, so this compiles on any platform and links
// against nothing. Whoever creates the context hands over its proc loader.
//
#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#define PG_GLAPI __stdcall
#else
#define PG_GLAPI
#endif

namespace pg::gl {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = unsigned char;
using GLchar = char;
using GLbitfield = unsigned int;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;

inline constexpr GLbitfield COLOR_BUFFER_BIT = 0x4000, DEPTH_BUFFER_BIT = 0x0100;
inline constexpr GLenum DEPTH_TEST = 0x0B71, CULL_FACE = 0x0B44, LESS = 0x0201;
inline constexpr GLenum TRIANGLES = 0x0004;
inline constexpr GLenum UNSIGNED_BYTE = 0x1401, UNSIGNED_INT = 0x1405, FLOAT = 0x1406;
inline constexpr GLenum ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893, STATIC_DRAW = 0x88E4;
inline constexpr GLenum VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, INFO_LOG_LENGTH = 0x8B84;
inline constexpr GLenum TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0;
inline constexpr GLenum RGBA = 0x1908, RGBA8 = 0x8058;
inline constexpr GLenum TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803;
inline constexpr GLint LINEAR = 0x2601, LINEAR_MIPMAP_LINEAR = 0x2703, REPEAT = 0x2901;
inline constexpr GLenum FRAMEBUFFER = 0x8D40, RENDERBUFFER = 0x8D41;
inline constexpr GLenum COLOR_ATTACHMENT0 = 0x8CE0, DEPTH_ATTACHMENT = 0x8D00;
inline constexpr GLenum DEPTH_COMPONENT24 = 0x81A6, FRAMEBUFFER_COMPLETE = 0x8CD5;
inline constexpr GLenum PACK_ALIGNMENT = 0x0D05, UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum RENDERER = 0x1F01, VERSION = 0x1F02;

// name, return type, parameters -- one list drives the struct and the loader.
#define PG_GL_FUNCTIONS(X)                                                                         \
    X(Viewport, void, (GLint, GLint, GLsizei, GLsizei))                                            \
    X(ClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat))                                      \
    X(Clear, void, (GLbitfield))                                                                   \
    X(Enable, void, (GLenum))                                                                      \
    X(Disable, void, (GLenum))                                                                     \
    X(DepthFunc, void, (GLenum))                                                                   \
    X(GetError, GLenum, ())                                                                        \
    X(GetString, const GLubyte*, (GLenum))                                                         \
    X(PixelStorei, void, (GLenum, GLint))                                                          \
    X(ReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                   \
    X(CreateShader, GLuint, (GLenum))                                                              \
    X(ShaderSource, void, (GLuint, GLsizei, const GLchar* const*, const GLint*))                   \
    X(CompileShader, void, (GLuint))                                                               \
    X(GetShaderiv, void, (GLuint, GLenum, GLint*))                                                 \
    X(GetShaderInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*))                                \
    X(DeleteShader, void, (GLuint))                                                                \
    X(CreateProgram, GLuint, ())                                                                   \
    X(AttachShader, void, (GLuint, GLuint))                                                        \
    X(LinkProgram, void, (GLuint))                                                                 \
    X(GetProgramiv, void, (GLuint, GLenum, GLint*))                                                \
    X(GetProgramInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*))                               \
    X(DeleteProgram, void, (GLuint))                                                               \
    X(UseProgram, void, (GLuint))                                                                  \
    X(GetUniformLocation, GLint, (GLuint, const GLchar*))                                          \
    X(Uniform1i, void, (GLint, GLint))                                                             \
    X(Uniform1f, void, (GLint, GLfloat))                                                           \
    X(Uniform2f, void, (GLint, GLfloat, GLfloat))                                                  \
    X(Uniform3f, void, (GLint, GLfloat, GLfloat, GLfloat))                                         \
    X(Uniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                \
    X(UniformMatrix4fv, void, (GLint, GLsizei, GLboolean, const GLfloat*))                         \
    X(GenVertexArrays, void, (GLsizei, GLuint*))                                                   \
    X(BindVertexArray, void, (GLuint))                                                             \
    X(DeleteVertexArrays, void, (GLsizei, const GLuint*))                                          \
    X(GenBuffers, void, (GLsizei, GLuint*))                                                        \
    X(BindBuffer, void, (GLenum, GLuint))                                                          \
    X(BufferData, void, (GLenum, GLsizeiptr, const void*, GLenum))                                 \
    X(DeleteBuffers, void, (GLsizei, const GLuint*))                                               \
    X(EnableVertexAttribArray, void, (GLuint))                                                     \
    X(VertexAttribPointer, void, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))         \
    X(DrawElements, void, (GLenum, GLsizei, GLenum, const void*))                                  \
    X(GenTextures, void, (GLsizei, GLuint*))                                                       \
    X(BindTexture, void, (GLenum, GLuint))                                                         \
    X(DeleteTextures, void, (GLsizei, const GLuint*))                                              \
    X(ActiveTexture, void, (GLenum))                                                               \
    X(TexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(TexParameteri, void, (GLenum, GLenum, GLint))                                                \
    X(GenerateMipmap, void, (GLenum))                                                              \
    X(GenFramebuffers, void, (GLsizei, GLuint*))                                                   \
    X(BindFramebuffer, void, (GLenum, GLuint))                                                     \
    X(DeleteFramebuffers, void, (GLsizei, const GLuint*))                                          \
    X(FramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint))                         \
    X(CheckFramebufferStatus, GLenum, (GLenum))                                                    \
    X(GenRenderbuffers, void, (GLsizei, GLuint*))                                                  \
    X(BindRenderbuffer, void, (GLenum, GLuint))                                                    \
    X(DeleteRenderbuffers, void, (GLsizei, const GLuint*))                                         \
    X(RenderbufferStorage, void, (GLenum, GLenum, GLsizei, GLsizei))                               \
    X(FramebufferRenderbuffer, void, (GLenum, GLenum, GLenum, GLuint))

using Proc = void (*)();
using GetProc = Proc (*)(const char* name);

struct Api {
#define PG_GL_MEMBER(name, ret, params) ret(PG_GLAPI* name) params = nullptr;
    PG_GL_FUNCTIONS(PG_GL_MEMBER)
#undef PG_GL_MEMBER

    /// Loads every entry point through `getProc` ("glClear", ...). False, with
    /// the first missing name in `missing`, if the context lacks one.
    bool load(GetProc getProc, std::string& missing);
};

}  // namespace pg::gl
