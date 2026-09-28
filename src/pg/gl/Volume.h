#pragma once
//
// Draws a simulation: the gas -- smoke that absorbs and scatters light, fire
// that glows -- and the water, standing on a floor, the objects of the scene,
// and guide lines on top. The viewport of the editor's Simulation network and
// of `prototype sim`.
//
// A fragment shader follows the ray behind each pixel:
//
//   solids   the floor (a grid on it, fading into the distance) and the
//            objects -- balls, boxes, columns, cones, rings, each turned and
//            sized, met exactly by the ray -- lit by the sun (in the shadow of
//            the smoke and of each other), the sky, and the glow of the fire;
//            a selected object glows at its rim;
//   water    the surface of its distance field, found by sphere tracing:
//            it reflects the sky, the sun and the objects, more at grazing
//            angles (Fresnel); the light that goes in bends (refraction) and
//            fades with the way it goes through the water, taking on its
//            colour; foam and spray are white; it shades what lies beneath;
//   rain     drops and the droplets they splash up, drawn as thin streaks
//            as long as they fall in a share of a frame (motion blur), after
//            the rest, hidden where something is in front of them; rings on
//            the water; a floor that the rain makes wet -- darker and shining;
//   geometry the network's displayed node (sim/Display.h), and the pieces
//            of an RBD Solver: polygons in their colours, lit as the objects
//            are; points as dots, polylines as lines, volumes as dots where
//            they are not empty;
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
#include "pg/core/Geometry.h"
#include "pg/core/Types.h"
#include "pg/gl/Camera.h"
#include "pg/gl/Gl.h"
#include "pg/sim/Camera.h"
#include "pg/sim/Display.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Look.h"
#include "pg/sim/Scene.h"
#include "pg/sim/World.h"

#include <memory>
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

/// The guides of a world: the domains of the gas and the water, their
/// sources, the forces, the rain's cloud, the camera, and the outlines of
/// the objects among `selected`. Those of the nodes in `selected` stand out.
/// The domains' boxes belong to `gasNode` and `waterNode`, the cloud to
/// `rainNode`: a click on one picks its node.
Lines sceneGuides(const sim::World* world, const std::vector<sim::Solid>& solids, const std::vector<int>& selected,
                  int gasNode = 0, int waterNode = 0, int rainNode = 0, const sim::Camera* camera = nullptr);
/// A box that holds every domain of `world`: what a camera should show.
sim::Domain sceneDomain(const sim::World& world);
/// The view through `camera`: an orbit round the point `distance` in front
/// of it, with its lens and its roll.
Orbit orbitThrough(const sim::Camera& camera, float distance);
/// `camera` moved and turned to see what `orbit` sees; its lens and picture
/// as they were.
sim::Camera cameraFrom(const Orbit& orbit, sim::Camera camera);

class VolumeRenderer;
/// The passes of the renderer's last render -- drawn with passes on --
/// averaged down 2x, to an OpenEXR file (pg/io/Exr.h): the picture in
/// linear light (R, G, B, A, halves), the depth along the view (Z, a float,
/// infinity where nothing is), the motion to the next frame in pixels, right
/// and up (forward.u, forward.v), a mask for each kind of surface and one
/// for the smoke (mask.*); `comment` in the header. False, with why.
bool writePassesExr(const VolumeRenderer& renderer, const std::string& path, const std::string& comment,
                    std::string& error);

class VolumeRenderer {
public:
    explicit VolumeRenderer(const Api& gl);
    ~VolumeRenderer();
    VolumeRenderer(const VolumeRenderer&) = delete;
    VolumeRenderer& operator=(const VolumeRenderer&) = delete;

    /// Compiles the shaders. False, with the driver's message in `log`, if not.
    bool init(std::string& log);

    /// The gas and the water to draw. Until the first frame -- or after
    /// clearFrame() -- the floor and the solids alone.
    void setFrame(const sim::Frame& frame);
    void clearFrame();
    /// Gas is drawn: the last frame had some.
    bool hasFrame() const { return hasFrame_; }
    bool hasWater() const { return hasWater_; }
    /// The domain the gas lives in: drawn where it is, even with no frame yet.
    void setDomain(const sim::Domain& domain);
    /// The objects of the scene, drawn and casting shadows; at most kMaxSolids.
    void setSolids(const std::vector<sim::Solid>& solids);
    /// The objects that stand out, by node: the selected ones, and the one
    /// under the mouse.
    void setHighlight(const std::vector<int>& selected, int hovered);
    /// Guide lines, drawn over the rest.
    void setLines(const Lines& lines);
    /// Geometry drawn with the scene: the network's displayed node. Null:
    /// none. The same geometry again costs nothing.
    void setGeometry(const GeometryPtr& geometry);
    const GeometryPtr& geometry() const { return geometry_; }
    /// The pieces of an RBD Solver, as its look draws them (sim::drawnPieces),
    /// drawn with the displayed geometry -- their loose points, the grit, as
    /// chips of stone. Null: none.
    void setPieces(const GeometryPtr& pieces);
    const GeometryPtr& pieces() const { return pieces_; }
    /// The box round the geometry drawn -- the displayed node's and the
    /// pieces; false when there is none.
    bool geometryBounds(Vec3& lo, Vec3& hi) const;

    /// The plate behind it all (sim::Camera::plate), filmed by `camera`: the
    /// picture of `file` fills that camera's frame; round it -- in the
    /// viewport -- the rest is drawn as it was. Over it, objects that are
    /// holdouts or catchers (sim::Matte) are the real things it shows. A
    /// picture as shown (PNG, JPEG) goes back through the renderer's view
    /// transform, so it comes out as it went in where nothing covers it;
    /// one in linear light (EXR) is taken as light. The same file again
    /// costs nothing. False, with why, for a file that cannot be read: no
    /// plate then. An empty `file`: none.
    bool setPlate(const std::string& file, const sim::Camera& camera, std::string& error);
    void clearPlate();
    bool hasPlate() const { return plateOn_ && plateTex_ != 0; }

    /// Draws into the offscreen framebuffer at `width` x `height` pixels.
    void render(int width, int height);

    GLuint colorTexture() const { return colorTex_; }
    int width() const { return width_; }
    int height() const { return height_; }
    /// The last frame as RGB rows, top to bottom, averaged down by `factor`.
    std::vector<uint8_t> readPixels(int factor = 1) const;

    /// The passes a compositor wants, drawn with the picture while `on`: the
    /// picture in linear light -- no tone curve, no gamma -- in 16-bit
    /// floats, and beside it the depth of the nearest surface, how much the
    /// smoke hides, what the surface is, and how far each pixel moves by the
    /// next frame: the displayed geometry and the pieces by their points'
    /// velocity v, everything by the camera -- `next` its view then.
    struct Passes {
        bool on = false;
        Orbit next;                      ///< the view of the next frame
        bool moving = false;             ///< false: the camera stands still
        float frameTime = 1.0f / 30.0f;  ///< seconds to the next frame
    };
    Passes passes;
    /// What a surface is, in the masks of the passes.
    enum class Surface { None, Floor, Geometry, Pieces, Objects, Water, Count };
    /// The passes of the last render, top line first, averaged over `factor`
    /// x `factor` pixels -- the depth the nearest of them.
    struct PassImage {
        int width = 0, height = 0;
        std::vector<float> rgba;    ///< four a pixel, linear
        std::vector<float> depth;   ///< along the view, world units; infinity where there is no surface
        std::vector<float> smoke;   ///< 0 to 1: how much of what is behind the smoke hides
        std::vector<float> motion;  ///< two a pixel: pixels right and up, to the next frame
        std::vector<float> masks[static_cast<int>(Surface::Count)];  ///< how much of each pixel is of each
        /// Drawn over a plate: rgba is the CG alone, alpha how much of the
        /// pixel it covers; and relit, three a pixel, what the plate is
        /// multiplied by where catchers relight it (1 elsewhere).
        bool plate = false;
        std::vector<float> relit;
    };
    PassImage readPasses(int factor = 1) const;

    /// A camera that shows the whole of `domain`, from a little above.
    static Orbit viewOf(const sim::Domain& domain);

    /// The view and projection the last render used, column-major: for
    /// picking, and for drawing over the image.
    const Mat4& viewProjection() const { return viewProjection_; }

    Orbit orbit;
    sim::Look look;

    static constexpr int kMaxSolids = 16;
    /// Meshes that cast shadows; more are drawn, without.
    static constexpr int kMaxMeshShadows = 4;
    /// The vertical field of view, degrees.
    static constexpr float kFovY = 35.0f;

private:
    void ensureTarget(int width, int height);
    /// The water of a frame to the GPU; none when it has none.
    void setWater(const sim::WaterFrame& water);
    /// The rain of a frame: a streak for each drop and droplet, and the
    /// ripples.
    void setRain(const sim::RainFrame& rain);
    /// The streaks, over what the main pass drew, behind what is in front.
    void drawRain(int width, int height, const Vec3& eye);
    /// The shadows of the smoke and the lamps of the fire, worked out again
    /// when the gas, the solids or the look they depend on change.
    void updateLighting();
    GLint location(GLuint program, const char* name) const;
    /// The box of the gas and the solids, for both programs.
    void setSceneUniforms(GLuint program);
    /// The meshes among the solids on the GPU: their triangles, and their
    /// distance fields for the shadows they cast.
    void syncMeshes();
    /// The meshes rasterised: normal, which solid and distance per pixel,
    /// for the pass that shades everything.
    void renderMeshes(int width, int height, const Vec3& eye);
    /// The displayed geometry's dots and lines, over what the main pass drew.
    void drawGeometry(int width, int height);
    /// The displayed geometry and the pieces, as they are drawn, to the GPU.
    void uploadGeometry();
    /// Their map from the sun -- `light` towards it -- for the shadows they
    /// cast: drawn again when they or the sun moved.
    void updateGeoShadow(const Vec3& light);

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
    GLuint rainProgram_ = 0, rainVao_ = 0, rainBuffer_ = 0, ripples_ = 0;
    GLsizei rainVertices_ = 0;
    bool hasRain_ = false, hasRipples_ = false;
    float rainTimeStep_ = 0.0f;
    Vec3 rippleMin_;
    float wetMin_[2] = {0.0f, 0.0f}, wetMax_[2] = {0.0f, 0.0f};  // where it rains, x and z
    float rippleCell_ = 0.0f;
    int rippleSize_[2] = {0, 0};
    GLuint water_ = 0;                          // 3D texture: distance to the surface, foam
    int waterSize_[3] = {0, 0, 0};
    bool hasWater_ = false;
    sim::Domain waterDomain_;
    float waterBand_ = 0.0f;
    std::vector<sim::Solid> solids_;
    std::vector<int> selected_;
    int hovered_ = 0;
    struct MeshGpu {
        std::shared_ptr<const sim::MeshShape> mesh;
        GLuint vao = 0, vbo = 0, sdf = 0;
        GLsizei vertices = 0;
    };
    std::vector<MeshGpu> meshes_;  // one per mesh in use, whoever uses it
    GLuint meshProgram_ = 0;
    // The displayed geometry: triangles (position, normal, colour), dots
    // (position, colour, radius), lines (as guide lines are).
    // The shadows the geometry casts: its depth seen from the sun.
    static constexpr int kGeoShadowSize = 2048;
    GLuint geoShadowProgram_ = 0, geoShadowFbo_ = 0, geoShadowTex_ = 0, geoShadowDepth_ = 0;
    bool geoShadowDirty_ = true, hasGeoShadow_ = false;
    Vec3 geoShadowLight_;
    Mat4 lightViewProj_{};
    float geoShadowBias_ = 0.0f;
    float geoShadowLift_ = 0.0f;
    GeometryPtr geometry_, pieces_;
    sim::DisplayGeometry shownDisplay_, piecesDisplay_;  // what each is drawn as
    GLuint geoProgram_ = 0, dotProgram_ = 0;
    GLuint geoVao_ = 0, geoBuffer_ = 0, dotVao_ = 0, dotBuffer_ = 0, curveVao_ = 0, curveBuffer_ = 0;
    GLsizei geoVertices_ = 0, dots_ = 0, curveVertices_ = 0;
    GLsizei gritDots_ = 0;  // the last of the dots: the pieces' loose points, their grit
    GLsizei shownVertices_ = 0;   // the first of the triangles: the displayed geometry's, then the pieces'
    GLuint geoVelocityBuffer_ = 0;  // the triangles' corners' velocities (attribute 3), when they have any
    // The passes: the targets beside the picture, and the meshes' motion.
    GLuint auxTex_[2] = {0, 0};
    bool targetPasses_ = false;
    GLuint gAux_ = 0;
    bool gPasses_ = false;
    Mat4 nextViewProjection_{};
    Vec3 geoLo_, geoHi_;
    bool hasGeoBounds_ = false;
    GLuint gFbo_ = 0, gTex_ = 0, gDepth_ = 0;
    int gWidth_ = 0, gHeight_ = 0;
    bool anyMesh_ = false;
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
    // The plate: its picture on the GPU, and the camera it fills.
    GLuint plateTex_ = 0;
    bool plateOn_ = false;
    std::string plateFile_;
    Vec3 plateForward_{0.0f, 0.0f, -1.0f}, plateRight_{1.0f, 0.0f, 0.0f}, plateUp_{0.0f, 1.0f, 0.0f};
    float plateTan_[2] = {1.0f, 1.0f};
};

}  // namespace pg::gl
