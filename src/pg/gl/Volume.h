#pragma once
//
// Draws a simulation: the gas -- smoke that absorbs and scatters light, fire
// that glows -- standing on a floor, the objects of the scene, and guide lines
// on top. The viewport of the editor's Simulation network and of `pgshader sim`.
//
// A fragment shader follows the ray behind each pixel:
//
//   solids   the floor (a grid on it, fading into the distance) and the
//            objects -- balls, boxes, columns, cones, rings, each turned and
//            sized, met exactly by the ray -- lit by the sun (in the shadow of
//            the smoke and of each other), the sky, and the glow of the fire;
//            a selected object glows at its rim;
//   gas      marched front to back through the domain, up to the first solid:
//            smoke absorbs what is behind it and scatters light towards the
//            eye -- sunlight where it is not in shadow, mostly forwards, so
//            smoke glows at the rim against the light; sky light, dimmer where
//            the smoke around is thick; the glow of the fire nearby. Fire
//            emits where the flame is, like a black body at the temperature
//            there: deep red, orange, then yellow-white as it heats up, and far
//            brighter (power goes with T^4).
//
// Then a filmic tone curve. The light that reaches each cell through the
// smoke -- the shadows it casts on itself -- is worked out on the GPU as well,
// a slice of a 3D texture at a time, whenever the gas, the light or the solids
// change. The shader writes the depth of the solids, so guide lines drawn
// afterwards pass behind them.
//
// Needs a current OpenGL 3.3 core context.
//
#include "pg/core/Types.h"
#include "pg/gl/Camera.h"
#include "pg/gl/Gl.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Look.h"
#include "pg/sim/Scene.h"

#include <string>
#include <vector>

namespace pg::gl {

/// A point of a guide line.
struct LineVertex {
    float position[3];
    float color[4];
};

/// Guide shapes, as pairs of line vertices. Each segment remembers the node
/// it was drawn for: a click near it picks that node.
struct Lines {
    std::vector<LineVertex> vertices;
    std::vector<int> owners;  ///< the node of each segment, one per two vertices
    int owner = 0;            ///< the node the segments added from now on belong to

    void segment(const Vec3& a, const Vec3& b, const float color[4]);
    void box(const Vec3& lo, const Vec3& hi, const float color[4]);
    void circle(const Vec3& center, const Vec3& axis, float radius, const float color[4], int segments = 48);
    /// Three circles, one round each axis.
    void sphere(const Vec3& center, float radius, const float color[4]);
    /// Two circles `height` apart about the center, and four lines between.
    void cylinder(const Vec3& center, const Vec3& axis, float radius, float height, const float color[4]);
    /// A line with a head at `to`.
    void arrow(const Vec3& from, const Vec3& to, const float color[4]);
    /// The outline of a placed shape, along its own axes: a ball's three
    /// rings, a box's edges, a column's two rims and four sides...
    void shape(const sim::ShapeInstance& shape, const float color[4]);
    void clear() {
        vertices.clear();
        owners.clear();
    }
};

/// The guides of a scene: the domain, the sources, the forces that act in a
/// region -- when there is a gas scene -- and the outlines of the objects
/// among `selected`. Those of the nodes in `selected` stand out.
/// The domain's box belongs to `domainNode`: a click on it picks the solver.
Lines sceneGuides(const sim::Scene* scene, const std::vector<sim::Solid>& solids, const std::vector<int>& selected,
                  int domainNode = 0);

class VolumeRenderer {
public:
    explicit VolumeRenderer(const Api& gl);
    ~VolumeRenderer();
    VolumeRenderer(const VolumeRenderer&) = delete;
    VolumeRenderer& operator=(const VolumeRenderer&) = delete;

    /// Compiles the shaders. False, with the driver's message in `log`, if not.
    bool init(std::string& log);

    /// The gas to draw. Until the first frame -- or after clearFrame() -- the
    /// floor and the solids alone.
    void setFrame(const sim::Frame& frame);
    void clearFrame();
    bool hasFrame() const { return hasFrame_; }
    /// The domain the gas lives in: drawn where it is, even with no frame yet.
    void setDomain(const sim::Domain& domain);
    /// The objects of the scene, drawn and casting shadows; at most kMaxSolids.
    void setSolids(const std::vector<sim::Solid>& solids);
    /// The objects that stand out, by node: the selected ones, and the one
    /// under the mouse.
    void setHighlight(const std::vector<int>& selected, int hovered);
    /// Guide lines, drawn over the rest.
    void setLines(const Lines& lines);

    /// Draws into the offscreen framebuffer at `width` x `height` pixels.
    void render(int width, int height);

    GLuint colorTexture() const { return colorTex_; }
    int width() const { return width_; }
    int height() const { return height_; }
    /// The last frame as RGB rows, top to bottom, averaged down by `factor`.
    std::vector<uint8_t> readPixels(int factor = 1) const;

    /// A camera that shows the whole of `domain`, from a little above.
    static Orbit viewOf(const sim::Domain& domain);

    /// The view and projection the last render used, column-major: for
    /// picking, and for drawing over the image.
    const Mat4& viewProjection() const { return viewProjection_; }

    Orbit orbit;
    sim::Look look;

    static constexpr int kMaxSolids = 16;
    /// The vertical field of view, degrees.
    static constexpr float kFovY = 35.0f;

private:
    void ensureTarget(int width, int height);
    /// The shadows of the smoke and the lamps of the fire, worked out again
    /// when the gas, the solids or the look they depend on change.
    void updateLighting();
    GLint location(GLuint program, const char* name) const;
    /// The box of the gas and the solids, for both programs.
    void setSceneUniforms(GLuint program);

    const Api& gl_;
    GLuint program_ = 0, shadowProgram_ = 0, glowProgram_ = 0, lineProgram_ = 0;
    GLuint vao_ = 0, lineVao_ = 0, lineBuffer_ = 0;
    GLuint fields_ = 0, light_ = 0, glow_ = 0;  // 3D textures
    GLuint passFbo_ = 0;                        // the target of the passes that fill light_ and glow_
    int size_[3] = {0, 0, 0}, lightSize_[3] = {0, 0, 0}, glowSize_[3] = {0, 0, 0};
    int glowBlock_ = 1;                         // gas cells in a block of glow_, along each axis
    GLuint fbo_ = 0, colorTex_ = 0, depthBuffer_ = 0;
    int width_ = 0, height_ = 0;
    sim::Domain domain_;
    bool hasFrame_ = false;
    std::vector<sim::Solid> solids_;
    std::vector<int> selected_;
    int hovered_ = 0;
    size_t lineCount_ = 0;
    // What the lighting was worked out for.
    struct LightingKey {
        Vec3 light;
        float density = -1.0f, flame = 0.0f, flameStart = 0.0f, flameRange = 0.0f;
        bool operator==(const LightingKey&) const = default;
    };
    bool lightingDirty_ = true;
    LightingKey lighting_;
    Mat4 viewProjection_{};
};

}  // namespace pg::gl
