#include "pg/gl/Preview.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace pg::gl {
namespace {

constexpr float kPi = 3.14159265358979323846f;

using Mat4 = std::array<float, 16>;  // column-major, as GL expects

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    }
    return r;
}

Mat4 perspective(float fovyDegrees, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovyDegrees * kPi / 360.0f);
    Mat4 m{};
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zFar + zNear) / (zNear - zFar);
    m[11] = -1.0f;
    m[14] = 2.0f * zFar * zNear / (zNear - zFar);
    return m;
}

Mat4 lookAt(const float eye[3]) {
    // Looking at the origin, y up.
    float f[3] = {-eye[0], -eye[1], -eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= fl;
    float s[3] = {f[1] * 0.0f - f[2] * 1.0f, f[2] * 0.0f - f[0] * 0.0f, f[0] * 1.0f - f[1] * 0.0f};
    const float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (float& v : s) v /= sl;
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4 m{};
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    m[15] = 1.0f;
    return m;
}

Mat4 identity() {
    Mat4 m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}

/// Interleaved position, normal, uv.
struct MeshData {
    std::vector<float> v;
    std::vector<uint32_t> i;

    void vertex(float px, float py, float pz, float nx, float ny, float nz, float u, float w) {
        v.insert(v.end(), {px, py, pz, nx, ny, nz, u, w});
    }
    uint32_t count() const { return static_cast<uint32_t>(v.size() / 8); }
    /// Quads of a (cols+1) x (rows+1) vertex grid starting at `base`.
    void grid(uint32_t base, int cols, int rows) {
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const uint32_t a = base + static_cast<uint32_t>(r * (cols + 1) + c);
                const uint32_t b = a + 1, d = a + static_cast<uint32_t>(cols + 1), e = d + 1;
                i.insert(i.end(), {a, b, e, a, e, d});
            }
        }
    }
};

MeshData sphere() {
    MeshData m;
    const int cols = 96, rows = 48;
    for (int r = 0; r <= rows; ++r) {
        const float v = static_cast<float>(r) / rows, phi = v * kPi;
        for (int c = 0; c <= cols; ++c) {
            const float u = static_cast<float>(c) / cols, theta = u * 2.0f * kPi;
            const float x = std::sin(phi) * std::sin(theta), y = -std::cos(phi), z = std::sin(phi) * std::cos(theta);
            m.vertex(x, y, z, x, y, z, u, v);
        }
    }
    m.grid(0, cols, rows);
    return m;
}

MeshData torus() {
    MeshData m;
    const int cols = 96, rows = 48;
    const float big = 0.75f, small = 0.32f;
    for (int r = 0; r <= rows; ++r) {
        const float v = static_cast<float>(r) / rows, phi = v * 2.0f * kPi;
        for (int c = 0; c <= cols; ++c) {
            const float u = static_cast<float>(c) / cols, theta = u * 2.0f * kPi;
            const float nx = std::cos(phi) * std::sin(theta), ny = std::sin(phi), nz = std::cos(phi) * std::cos(theta);
            m.vertex((big + small * std::cos(phi)) * std::sin(theta), small * ny,
                     (big + small * std::cos(phi)) * std::cos(theta), nx, ny, nz, u * 3.0f, v);
        }
    }
    m.grid(0, cols, rows);
    return m;
}

MeshData cube() {
    MeshData m;
    const int n = 24;
    // Each face: normal, then the directions its u and v run along.
    const float faces[6][9] = {
        {0, 0, 1, 1, 0, 0, 0, 1, 0},  {0, 0, -1, -1, 0, 0, 0, 1, 0}, {1, 0, 0, 0, 0, -1, 0, 1, 0},
        {-1, 0, 0, 0, 0, 1, 0, 1, 0}, {0, 1, 0, 1, 0, 0, 0, 0, -1},  {0, -1, 0, 1, 0, 0, 0, 0, 1},
    };
    for (const auto& f : faces) {
        const uint32_t base = m.count();
        for (int r = 0; r <= n; ++r) {
            for (int c = 0; c <= n; ++c) {
                const float u = static_cast<float>(c) / n, v = static_cast<float>(r) / n;
                const float h = 0.62f;  // half the edge
                const float a = (u * 2.0f - 1.0f) * h, b = (v * 2.0f - 1.0f) * h;
                m.vertex(f[0] * h + f[3] * a + f[6] * b, f[1] * h + f[4] * a + f[7] * b,
                         f[2] * h + f[5] * a + f[8] * b, f[0], f[1], f[2], u, v);
            }
        }
        m.grid(base, n, n);
    }
    return m;
}

MeshData plane() {
    MeshData m;
    const int n = 96;
    for (int r = 0; r <= n; ++r) {
        for (int c = 0; c <= n; ++c) {
            const float u = static_cast<float>(c) / n, v = static_cast<float>(r) / n;
            m.vertex((u - 0.5f) * 2.0f, 0.0f, (0.5f - v) * 2.0f, 0.0f, 1.0f, 0.0f, u, v);
        }
    }
    m.grid(0, n, n);
    return m;
}

/// An upright square facing the eye, turned about the vertical axis only, so
/// that flames keep pointing up. u runs left to right, v bottom to top.
MeshData billboard(const float eye[3]) {
    float rx = eye[2], rz = -eye[0];  // horizontal, perpendicular to the view
    const float len = std::sqrt(rx * rx + rz * rz);
    if (len < 1e-5f) {
        rx = 1.0f;
        rz = 0.0f;
    } else {
        rx /= len;
        rz /= len;
    }
    const float nx = -rz, nz = rx;  // towards the eye
    const float h = 0.95f;          // half the edge
    MeshData m;
    for (int r = 0; r <= 1; ++r) {
        for (int c = 0; c <= 1; ++c) {
            const float a = (c * 2.0f - 1.0f) * h, b = (r * 2.0f - 1.0f) * h;
            m.vertex(rx * a, b, rz * a, nx, 0.0f, nz, static_cast<float>(c), static_cast<float>(r));
        }
    }
    m.grid(0, 1, 1);
    return m;
}

/// A UV test image: hue across u, brightness up v, a grid every 1/8.
std::vector<uint8_t> testImage(int size) {
    std::vector<uint8_t> px(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            const float h = u * 6.0f;
            const float r = std::clamp(std::fabs(h - 3.0f) - 1.0f, 0.0f, 1.0f);
            const float g = std::clamp(2.0f - std::fabs(h - 2.0f), 0.0f, 1.0f);
            const float b = std::clamp(2.0f - std::fabs(h - 4.0f), 0.0f, 1.0f);
            const float light = 0.35f + 0.65f * v;
            const bool line = std::fmod(u * 8.0f, 1.0f) < 0.04f || std::fmod(v * 8.0f, 1.0f) < 0.04f;
            const bool check = (static_cast<int>(u * 8.0f) + static_cast<int>(v * 8.0f)) % 2 == 0;
            const float k = line ? 0.1f : (check ? 1.0f : 0.82f);
            uint8_t* p = &px[(static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4];
            p[0] = static_cast<uint8_t>(255.0f * std::clamp((0.25f + 0.75f * r) * light * k, 0.0f, 1.0f));
            p[1] = static_cast<uint8_t>(255.0f * std::clamp((0.25f + 0.75f * g) * light * k, 0.0f, 1.0f));
            p[2] = static_cast<uint8_t>(255.0f * std::clamp((0.25f + 0.75f * b) * light * k, 0.0f, 1.0f));
            p[3] = 255;
        }
    }
    return px;
}

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

const char* meshName(MeshKind kind) {
    switch (kind) {
        case MeshKind::Sphere: return "sphere";
        case MeshKind::Torus:  return "torus";
        case MeshKind::Cube:   return "cube";
        case MeshKind::Plane:  return "plane";
        case MeshKind::Billboard: return "billboard";
    }
    return "?";
}

PreviewRenderer::PreviewRenderer(const Api& gl) : gl_(gl) {
    gl_.GenVertexArrays(1, &vao_);
    gl_.GenBuffers(1, &vbo_);
    gl_.GenBuffers(1, &ibo_);

    const int size = 512;
    const std::vector<uint8_t> image = testImage(size);
    gl_.GenTextures(1, &testTexture_);
    gl_.BindTexture(TEXTURE_2D, testTexture_);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 1);
    gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA8), size, size, 0, RGBA, UNSIGNED_BYTE, image.data());
    gl_.GenerateMipmap(TEXTURE_2D);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR_MIPMAP_LINEAR);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, REPEAT);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, REPEAT);
}

PreviewRenderer::~PreviewRenderer() {
    if (program_) gl_.DeleteProgram(program_);
    gl_.DeleteVertexArrays(1, &vao_);
    gl_.DeleteBuffers(1, &vbo_);
    gl_.DeleteBuffers(1, &ibo_);
    gl_.DeleteTextures(1, &testTexture_);
    if (fbo_) {
        gl_.DeleteFramebuffers(1, &fbo_);
        gl_.DeleteTextures(1, &colorTex_);
        gl_.DeleteRenderbuffers(1, &depthRb_);
    }
}

bool PreviewRenderer::setProgram(const std::string& vertex, const std::string& fragment,
                                 std::string& log) {
    log.clear();
    const GLuint vs = compile(gl_, VERTEX_SHADER, vertex, log);
    const GLuint fs = compile(gl_, FRAGMENT_SHADER, fragment, log);
    if (!vs || !fs) {
        if (vs) gl_.DeleteShader(vs);
        if (fs) gl_.DeleteShader(fs);
        return false;
    }
    const GLuint p = gl_.CreateProgram();
    gl_.AttachShader(p, vs);
    gl_.AttachShader(p, fs);
    gl_.LinkProgram(p);
    gl_.DeleteShader(vs);
    gl_.DeleteShader(fs);
    GLint ok = 0;
    gl_.GetProgramiv(p, LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        gl_.GetProgramiv(p, INFO_LOG_LENGTH, &len);
        std::string msg(static_cast<size_t>(std::max(len, 1)), '\0');
        gl_.GetProgramInfoLog(p, len, nullptr, msg.data());
        log = "link: " + std::string(msg.c_str());
        gl_.DeleteProgram(p);
        return false;
    }
    if (program_) gl_.DeleteProgram(program_);
    program_ = p;
    locations_.clear();
    return true;
}

void PreviewRenderer::setUniforms(const std::vector<shader::UniformInfo>& uniforms) {
    uniforms_ = uniforms;
}

void PreviewRenderer::setUniformValue(const std::string& name, const shader::Value& value) {
    overrides_[name] = value;
}

void PreviewRenderer::setMesh(MeshKind kind) {
    if (kind == meshKind_) return;
    meshKind_ = kind;
    meshDirty_ = true;
}

GLint PreviewRenderer::location(const std::string& name) {
    auto it = locations_.find(name);
    if (it != locations_.end()) return it->second;
    const GLint loc = gl_.GetUniformLocation(program_, name.c_str());
    locations_[name] = loc;
    return loc;
}

void PreviewRenderer::uploadMesh(const float eye[3]) {
    MeshData m;
    switch (meshKind_) {
        case MeshKind::Sphere: m = sphere(); break;
        case MeshKind::Torus:  m = torus(); break;
        case MeshKind::Cube:   m = cube(); break;
        case MeshKind::Plane:  m = plane(); break;
        case MeshKind::Billboard: m = billboard(eye); break;
    }
    gl_.BindVertexArray(vao_);
    gl_.BindBuffer(ARRAY_BUFFER, vbo_);
    gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(m.v.size() * sizeof(float)), m.v.data(), STATIC_DRAW);
    gl_.BindBuffer(ELEMENT_ARRAY_BUFFER, ibo_);
    gl_.BufferData(ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(m.i.size() * sizeof(uint32_t)), m.i.data(),
                   STATIC_DRAW);
    const GLsizei stride = 8 * sizeof(float);
    for (GLuint a = 0; a < 3; ++a) gl_.EnableVertexAttribArray(a);
    gl_.VertexAttribPointer(0, 3, FLOAT, 0, stride, reinterpret_cast<const void*>(0));
    gl_.VertexAttribPointer(1, 3, FLOAT, 0, stride, reinterpret_cast<const void*>(3 * sizeof(float)));
    gl_.VertexAttribPointer(2, 2, FLOAT, 0, stride, reinterpret_cast<const void*>(6 * sizeof(float)));
    gl_.BindVertexArray(0);
    indexCount_ = static_cast<GLsizei>(m.i.size());
    meshDirty_ = false;
}

void PreviewRenderer::ensureTarget(int width, int height) {
    if (fbo_ && width == width_ && height == height_) return;
    if (!fbo_) {
        gl_.GenFramebuffers(1, &fbo_);
        gl_.GenTextures(1, &colorTex_);
        gl_.GenRenderbuffers(1, &depthRb_);
    }
    width_ = width;
    height_ = height;
    gl_.BindTexture(TEXTURE_2D, colorTex_);
    gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA8), width, height, 0, RGBA, UNSIGNED_BYTE, nullptr);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
    gl_.BindRenderbuffer(RENDERBUFFER, depthRb_);
    gl_.RenderbufferStorage(RENDERBUFFER, DEPTH_COMPONENT24, width, height);
    gl_.BindFramebuffer(FRAMEBUFFER, fbo_);
    gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, colorTex_, 0);
    gl_.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, depthRb_);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
}

void PreviewRenderer::render(int width, int height, float time) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    ensureTarget(width, height);
    const float yaw = orbit.yaw * kPi / 180.0f, pitch = orbit.pitch * kPi / 180.0f;
    const float eye[3] = {orbit.distance * std::cos(pitch) * std::sin(yaw), orbit.distance * std::sin(pitch),
                          orbit.distance * std::cos(pitch) * std::cos(yaw)};
    // A billboard follows the camera, so it is rebuilt every frame: four vertices.
    if (meshDirty_ || meshKind_ == MeshKind::Billboard) uploadMesh(eye);

    gl_.BindFramebuffer(FRAMEBUFFER, fbo_);
    gl_.Viewport(0, 0, width, height);
    gl_.ClearColor(background[0], background[1], background[2], 1.0f);
    gl_.Clear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
    if (program_) {
        gl_.Enable(DEPTH_TEST);
        gl_.DepthFunc(LESS);
        // The image stays opaque whatever alpha the shader writes: it is shown
        // in a UI that blends, and the background is part of the preview.
        gl_.ColorMask(1, 1, 1, 0);
        if (blend_ != shader::BlendMode::Opaque) {
            gl_.Enable(BLEND);
            gl_.BlendFunc(SRC_ALPHA, blend_ == shader::BlendMode::Additive ? ONE : ONE_MINUS_SRC_ALPHA);
            gl_.DepthMask(0);  // see-through surfaces do not hide what is behind them
        }
        gl_.UseProgram(program_);

        const Mat4 viewProj = multiply(
            perspective(35.0f, static_cast<float>(width) / static_cast<float>(height), 0.05f, 50.0f), lookAt(eye));
        const Mat4 model = identity();
        const float* L = lightDirection;
        const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);

        gl_.UniformMatrix4fv(location("u_model"), 1, 0, model.data());
        gl_.UniformMatrix4fv(location("u_viewProj"), 1, 0, viewProj.data());
        gl_.Uniform3f(location("u_cameraPos"), eye[0], eye[1], eye[2]);
        gl_.Uniform3f(location("u_lightDir"), L[0] / ll, L[1] / ll, L[2] / ll);
        gl_.Uniform1f(location("u_time"), time);

        for (const auto& u : uniforms_) {
            const GLint loc = location(u.name);
            if (loc < 0) continue;
            if (u.type == shader::Type::Sampler2D) {
                gl_.ActiveTexture(TEXTURE0 + static_cast<GLenum>(u.binding));
                gl_.BindTexture(TEXTURE_2D, testTexture_);
                gl_.Uniform1i(loc, u.binding);
                continue;
            }
            auto it = overrides_.find(u.name);
            const shader::Value v = (it != overrides_.end() ? it->second : u.defaultValue).as(u.type);
            switch (u.type) {
                case shader::Type::Float: gl_.Uniform1f(loc, v.v[0]); break;
                case shader::Type::Vec2:  gl_.Uniform2f(loc, v.v[0], v.v[1]); break;
                case shader::Type::Vec3:  gl_.Uniform3f(loc, v.v[0], v.v[1], v.v[2]); break;
                case shader::Type::Vec4:  gl_.Uniform4f(loc, v.v[0], v.v[1], v.v[2], v.v[3]); break;
                default: break;
            }
        }
        gl_.ActiveTexture(TEXTURE0);

        gl_.BindVertexArray(vao_);
        gl_.DrawElements(TRIANGLES, indexCount_, UNSIGNED_INT, nullptr);
        gl_.BindVertexArray(0);
        gl_.UseProgram(0);
        gl_.Disable(BLEND);
        gl_.DepthMask(1);
        gl_.ColorMask(1, 1, 1, 1);
        gl_.Disable(DEPTH_TEST);
    }
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
}

std::vector<uint8_t> PreviewRenderer::readPixels(int factor) const {
    factor = std::max(factor, 1);
    std::vector<uint8_t> rgba(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4);
    gl_.BindFramebuffer(FRAMEBUFFER, fbo_);
    gl_.PixelStorei(PACK_ALIGNMENT, 1);
    gl_.ReadPixels(0, 0, width_, height_, RGBA, UNSIGNED_BYTE, rgba.data());
    gl_.BindFramebuffer(FRAMEBUFFER, 0);

    // GL rows run bottom to top; images top to bottom. Average factor x factor.
    const int w = width_ / factor, h = height_ / factor;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                int sum = 0;
                for (int dy = 0; dy < factor; ++dy) {
                    for (int dx = 0; dx < factor; ++dx) {
                        const int sy = height_ - 1 - (y * factor + dy), sx = x * factor + dx;
                        sum += rgba[(static_cast<size_t>(sy) * static_cast<size_t>(width_) + static_cast<size_t>(sx)) * 4 +
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
