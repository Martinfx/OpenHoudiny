#pragma once
//
// Draws a generated shader onto a preview mesh -- the "material ball".
//
// Used by the editor, which shows the colour texture in its UI, and by
// `pgshader render`, which reads the pixels back into a PNG without a window.
// Needs a current OpenGL 3.3 core context and the Api loaded from it. Feeds the
// built-in uniforms (u_model, u_viewProj, u_cameraPos, u_lightDir, u_time) and
// the graph's own: numbers start at their defaults, every texture gets a UV
// test image.
//
#include "pg/gl/Gl.h"
#include "pg/shader/Target.h"

#include <map>
#include <string>
#include <vector>

namespace pg::gl {

enum class MeshKind { Sphere, Torus, Cube, Plane };
inline constexpr MeshKind kMeshKinds[] = {MeshKind::Sphere, MeshKind::Torus, MeshKind::Cube,
                                          MeshKind::Plane};
const char* meshName(MeshKind kind);

/// Camera orbiting the origin.
struct Orbit {
    float yaw = 30.0f;    ///< degrees around the vertical axis
    float pitch = 18.0f;  ///< degrees above the horizon
    float distance = 3.4f;
};

class PreviewRenderer {
public:
    explicit PreviewRenderer(const Api& gl);
    ~PreviewRenderer();
    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

    /// Compiles and links GLSL 330 sources. On failure the previous program
    /// stays in use and `log` holds the driver's message.
    bool setProgram(const std::string& vertex, const std::string& fragment, std::string& log);
    bool hasProgram() const { return program_ != 0; }

    /// The graph's uniforms, as the generator reported them.
    void setUniforms(const std::vector<shader::UniformInfo>& uniforms);
    /// Overrides one numeric uniform -- what a slider in the editor does.
    void setUniformValue(const std::string& name, const shader::Value& value);

    void setMesh(MeshKind kind);
    MeshKind mesh() const { return meshKind_; }

    /// Draws into the offscreen framebuffer at `width` x `height` pixels.
    void render(int width, int height, float time);

    GLuint colorTexture() const { return colorTex_; }
    int width() const { return width_; }
    int height() const { return height_; }

    /// The last frame as RGB rows, top to bottom, averaged down by `factor`
    /// (render at 2x and read with 2 for anti-aliasing).
    std::vector<uint8_t> readPixels(int factor = 1) const;

    Orbit orbit;
    float lightDirection[3] = {0.5f, 0.75f, 0.45f};  ///< towards the light
    float background[3] = {0.16f, 0.17f, 0.19f};

private:
    void uploadMesh();
    void ensureTarget(int width, int height);
    GLint location(const std::string& name);

    const Api& gl_;
    GLuint program_ = 0;
    std::map<std::string, GLint> locations_;
    std::vector<shader::UniformInfo> uniforms_;
    std::map<std::string, shader::Value> overrides_;

    MeshKind meshKind_ = MeshKind::Sphere;
    GLuint vao_ = 0, vbo_ = 0, ibo_ = 0;
    GLsizei indexCount_ = 0;
    bool meshDirty_ = true;

    GLuint fbo_ = 0, colorTex_ = 0, depthRb_ = 0;
    int width_ = 0, height_ = 0;
    GLuint testTexture_ = 0;
};

}  // namespace pg::gl
