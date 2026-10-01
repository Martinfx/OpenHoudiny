#include "pg/render/Cycles.h"

#include "pg/render/Textures.h"

#include <glm/common.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#ifdef PG_HAVE_CYCLES
// The nodes first, whole: a socket's setter in another node's header that
// takes one of them, a Shader * or a Geometry *, compiled where the type is
// only declared, would take the pointer for a bool (Node::set's overload) --
// as Cycles' own sources have it, the types they set are known before.
#include "scene/shader.h"
#include "scene/geometry.h"
#include "scene/mesh.h"
#include "scene/light.h"
#include "scene/object.h"
#include "scene/background.h"
#include "scene/camera.h"
#include "scene/film.h"
#include "scene/image.h"
#include "scene/integrator.h"
#include "scene/pass.h"
#include "scene/scene.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "device/device.h"
#include "session/buffers.h"
#include "session/display_driver.h"
#include "session/output_driver.h"
#include "session/session.h"
#include "util/version.h"
#endif

#ifdef PG_HAVE_CYCLES
// Cycles' sky model (its third_party/sky), linked with it: the light of
// Nishita's sun at the bottom and the top of its disc, CIE XYZ.
extern "C" void SKY_nishita_skymodel_precompute_sun(float sun_elevation, float angular_diameter, float altitude,
                                                    float air_density, float dust_density, float* r_pixel_bottom,
                                                    float* r_pixel_top);
#endif

namespace pg::render {

#ifdef PG_HAVE_CYCLES
namespace {

constexpr float kPi = 3.14159265358979f;
/// The most cells of the gas Cycles is given: more, and they are read in
/// blocks (Gas::dense) -- 128 MB of grid.
constexpr size_t kMostGasCells = size_t(32) << 20;

// Ours is Y up, Cycles' Z up: the scene turned a quarter about x for it,
// (x, y, z) -> (x, -z, y).
ccl::float3 toCycles(const Vec3& v) { return ccl::make_float3(v.x, -v.z, v.y); }
Vec3 fromCycles(const float* v) { return Vec3(v[0], v[2], -v[1]); }
ccl::float3 rgb(const Vec3& c) { return ccl::make_float3(c.x, c.y, c.z); }

/// Ours, a mesh's space placed in the world -- its axes the columns of
/// `axes` times `scale`, its origin at `at` -- as Cycles' world sees it.
ccl::Transform placement(const Mat3& axes, float scale, const Vec3& at) {
    const Mat3 m = axes * scale;
    return ccl::make_transform(m[0].x, m[1].x, m[2].x, at.x,      //
                               -m[0].z, -m[1].z, -m[2].z, -at.z,  //
                               m[0].y, m[1].y, m[2].y, at.y);
}

/// The look's sky as a picture all round, as Cycles maps one onto the
/// world (equirectangular, Z up): Cycles samples it as it lights the scene.
class SkyImage : public ccl::ImageLoader {
public:
    SkyImage(std::vector<float> rgba, int width, int height, uint64_t id)
        : rgba_(std::move(rgba)), width_(width), height_(height), id_(id) {}

    bool load_metadata(const ccl::ImageDeviceFeatures&, ccl::ImageMetaData& m) override {
        m.width = static_cast<size_t>(width_);
        m.height = static_cast<size_t>(height_);
        m.depth = 1;
        m.channels = 4;
        m.type = ccl::IMAGE_DATA_TYPE_FLOAT4;
        m.colorspace = ccl::u_colorspace_raw;  // linear light, as it is
        return true;
    }
    /// `size`: how many floats, not bytes.
    bool load_pixels(const ccl::ImageMetaData&, void* pixels, const size_t size, const bool) override {
        std::memcpy(pixels, rgba_.data(), std::min(size, rgba_.size()) * sizeof(float));
        return true;
    }
    std::string name() const override { return "the look's sky"; }
    bool equals(const ccl::ImageLoader& other) const override {
        const auto* sky = dynamic_cast<const SkyImage*>(&other);
        return sky && sky->id_ == id_;
    }

private:
    std::vector<float> rgba_;
    int width_, height_;
    uint64_t id_;
};

/// The gas as Cycles reads a grid: its cells' numbers -- one float a cell,
/// or four -- as a picture in three dimensions, x fastest.
class VoxelImage : public ccl::ImageLoader {
public:
    /// `where`: from a point of Cycles' world to the grid's 0 to 1.
    VoxelImage(std::vector<float> values, int channels, const int size[3], const ccl::Transform& where, uint64_t id)
        : values_(std::move(values)), channels_(channels), size_{size[0], size[1], size[2]}, where_(where), id_(id) {}

    bool load_metadata(const ccl::ImageDeviceFeatures&, ccl::ImageMetaData& m) override {
        m.width = static_cast<size_t>(size_[0]);
        m.height = static_cast<size_t>(size_[1]);
        m.depth = static_cast<size_t>(size_[2]);
        m.channels = channels_;
        m.type = channels_ == 1 ? ccl::IMAGE_DATA_TYPE_FLOAT : ccl::IMAGE_DATA_TYPE_FLOAT4;
        m.colorspace = ccl::u_colorspace_raw;
        m.transform_3d = where_;
        m.use_transform_3d = true;
        return true;
    }
    /// `size`: how many floats.
    bool load_pixels(const ccl::ImageMetaData&, void* pixels, const size_t size, const bool) override {
        std::memcpy(pixels, values_.data(), std::min(size, values_.size()) * sizeof(float));
        return true;
    }
    std::string name() const override { return "the gas"; }
    bool equals(const ccl::ImageLoader& other) const override {
        const auto* voxels = dynamic_cast<const VoxelImage*>(&other);
        return voxels && voxels->id_ == id_;
    }

private:
    std::vector<float> values_;
    int channels_;
    int size_[3];
    ccl::Transform where_;
    uint64_t id_;
};

/// A picture -- and, at the end, what the pixels see -- out of Cycles'
/// buffers: bottom row first there, top row first here, the normals turned
/// back to Y up.
struct Pictures {
    std::mutex mutex;
    Image beauty, albedo, normal, depth;
    bool fresh = false;
    bool denoise = false;
    // The lens: tangents of half the angle across and up the picture --
    // Cycles' depth is along the camera's axis, ours along the ray.
    float tanX = 1.0f, tanY = 1.0f;
};

class Output : public ccl::OutputDriver {
public:
    explicit Output(Pictures& out) : out_(out) {}

    void write_render_tile(const Tile& tile) override { read(tile, true); }

private:
    enum class As { Light, Normal, Depth };

    void read(const Tile& tile, bool final) {
        const int w = tile.size.x, h = tile.size.y;
        if (w <= 0 || h <= 0) return;
        // A tile of a larger picture (a final render in tiles) goes where
        // it belongs; a picture of fewer pixels (the first, interactive)
        // stands for all of it.
        const bool part = final && !(tile.size == tile.full_size);
        const int fw = part ? tile.full_size.x : w, fh = part ? tile.full_size.y : h;
        const int ox = part ? tile.offset.x : 0, oy = part ? tile.offset.y : 0;
        std::vector<float> buffer(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        auto pass = [&](const char* name, int channels, Image& into, As as) {
            if (!tile.get_pass_pixels(name, channels, buffer.data())) return;
            if (into.width != fw || into.height != fh || into.channels != std::min(channels, 3)) {
                into.width = fw;
                into.height = fh;
                into.channels = std::min(channels, 3);
                into.pixels.assign(static_cast<size_t>(fw) * static_cast<size_t>(fh) * static_cast<size_t>(into.channels), 0.0f);
            }
            for (int y = 0; y < h; ++y) {
                // Cycles' row y counts from the bottom.
                const int row = fh - 1 - (oy + y);
                const float v = (1.0f - 2.0f * (static_cast<float>(row) + 0.5f) / static_cast<float>(fh)) * out_.tanY;
                for (int x = 0; x < w; ++x) {
                    const float* p = &buffer[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) *
                                             static_cast<size_t>(channels)];
                    float* q = &into.pixels[(static_cast<size_t>(row) * static_cast<size_t>(fw) + static_cast<size_t>(ox + x)) *
                                            static_cast<size_t>(into.channels)];
                    if (as == As::Normal) {
                        const Vec3 n = fromCycles(p);
                        q[0] = n.x;
                        q[1] = n.y;
                        q[2] = n.z;
                    } else if (as == As::Depth) {
                        // Along the ray, as ours: the depth over the cosine
                        // of the angle off the axis; what is not there,
                        // infinitely far.
                        const float u = (2.0f * (static_cast<float>(ox + x) + 0.5f) / static_cast<float>(fw) - 1.0f) * out_.tanX;
                        q[0] = p[0] > 0.0f && p[0] < 1e9f ? p[0] * std::sqrt(1.0f + u * u + v * v)
                                                          : std::numeric_limits<float>::infinity();
                    } else {
                        for (int c = 0; c < into.channels; ++c) q[c] = p[c];
                    }
                }
            }
        };
        std::lock_guard<std::mutex> lock(out_.mutex);
        pass("combined", 4, out_.beauty, As::Light);
        if (final) {
            pass("albedo", 3, out_.albedo, As::Light);
            pass("normal", 3, out_.normal, As::Normal);
            pass("depth", 1, out_.depth, As::Depth);
        }
        out_.fresh = true;
    }

    Pictures& out_;
};

/// IEEE 754 half to float.
float fromHalf(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1fu, mantissa = h & 0x3ffu;
    uint32_t bits;
    if (exponent == 0x1fu) {
        bits = sign | 0x7f800000u | (mantissa << 13);  // inf, NaN
    } else if (exponent != 0) {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    } else if (mantissa == 0) {
        bits = sign;
    } else {
        // Subnormal: made normal.
        exponent = 113;
        while (!(mantissa & 0x400u)) {
            mantissa <<= 1;
            --exponent;
        }
        bits = sign | (exponent << 23) | ((mantissa & 0x3ffu) << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

/// The pictures of an interactive render as Cycles shows them as it goes:
/// the first of fewer, larger pixels -- the size it hands over -- then
/// sharper; its light, linear, in half floats. Nothing drawn: they go to
/// the Render tab.
class Display : public ccl::DisplayDriver {
public:
    explicit Display(Pictures& out) : out_(out) {}

    void next_tile_begin() override {}
    bool update_begin(const Params&, const int width, const int height) override {
        width_ = std::max(width, 0);
        height_ = std::max(height, 0);
        pixels_.resize(static_cast<size_t>(width_) * static_cast<size_t>(height_));
        return width_ > 0 && height_ > 0;
    }
    void update_end() override {
        Image image;
        image.width = width_;
        image.height = height_;
        image.channels = 3;
        image.pixels.resize(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3);
        for (int y = 0; y < height_; ++y) {
            // Cycles' row y counts from the bottom.
            const ccl::half4* row = &pixels_[static_cast<size_t>(height_ - 1 - y) * static_cast<size_t>(width_)];
            float* q = &image.pixels[static_cast<size_t>(y) * static_cast<size_t>(width_) * 3];
            for (int x = 0; x < width_; ++x) {
                ccl::half4 p = row[x];
                q[3 * x] = fromHalf(static_cast<unsigned short>(p.x));
                q[3 * x + 1] = fromHalf(static_cast<unsigned short>(p.y));
                q[3 * x + 2] = fromHalf(static_cast<unsigned short>(p.z));
            }
        }
        std::lock_guard<std::mutex> lock(out_.mutex);
        out_.beauty = std::move(image);
        out_.fresh = true;
    }
    ccl::half4* map_texture_buffer() override { return pixels_.data(); }
    void unmap_texture_buffer() override {}
    void zero() override { std::fill(pixels_.begin(), pixels_.end(), ccl::half4{}); }
    void draw(const Params&) override {}

private:
    Pictures& out_;
    std::vector<ccl::half4> pixels_;
    int width_ = 0, height_ = 0;
};

/// A Principled BSDF as Blender makes one: its reflection as strong as
/// its IOR has it -- left to itself, Cycles' node reflects nothing.
ccl::PrincipledBsdfNode* principled(ccl::ShaderGraph& graph) {
    auto* bsdf = graph.create_node<ccl::PrincipledBsdfNode>();
    bsdf->set_specular_ior_level(0.5f);
    return bsdf;
}

/// `surface` -- glass, water -- but to the shadows: clear, as much of the
/// light through as `through` (or `amount`) says. Ours lets the sun through
/// them so; Cycles would bring it only along the caustics it finds, and
/// leave a room behind a window, a pool's floor, in the dark.
ccl::ShaderOutput* throughForShadows(ccl::ShaderGraph& graph, ccl::ShaderOutput* surface, ccl::ShaderOutput* through,
                                     float amount = 1.0f) {
    auto* path = graph.create_node<ccl::LightPathNode>();
    auto* clear = graph.create_node<ccl::TransparentBsdfNode>();
    clear->set_color(ccl::make_float3(amount, amount, amount));
    if (through) graph.connect(through, clear->input("Color"));
    auto* mix = graph.create_node<ccl::MixClosureNode>();
    graph.connect(path->output("Is Shadow Ray"), mix->input("Fac"));
    graph.connect(surface, mix->input("Closure1"));
    graph.connect(clear->output("BSDF"), mix->input("Closure2"));
    return mix->output("Closure");
}

/// Perlin's noise (fBM) at `at`, `scale` times a unit across, of `detail`
/// octaves: 0 to 1, a half on the whole.
ccl::ShaderOutput* noise(ccl::ShaderGraph& graph, ccl::ShaderOutput* at, float scale, float detail, float roughness) {
    auto* n = graph.create_node<ccl::NoiseTextureNode>();
    n->set_scale(scale);
    n->set_detail(detail);
    n->set_roughness(roughness);
    if (at) graph.connect(at, n->input("Vector"));
    return n->output("Fac");
}

/// `x` times `k`, plus `add` -- or plus what `plus` gives; clamped to 0..1
/// with `clamp`.
ccl::ShaderOutput* scaled(ccl::ShaderGraph& graph, ccl::ShaderOutput* x, float k, float add,
                          ccl::ShaderOutput* plus = nullptr, bool clamp = false) {
    auto* m = graph.create_node<ccl::MathNode>();
    m->set_math_type(ccl::NODE_MATH_MULTIPLY_ADD);
    graph.connect(x, m->input("Value1"));
    m->set_value2(k);
    m->set_value3(add);
    m->set_use_clamp(clamp && !plus);
    if (!plus) return m->output("Value");
    // The third input linked would be `plus` instead of `add`: added after.
    auto* sum = graph.create_node<ccl::MathNode>();
    sum->set_math_type(ccl::NODE_MATH_ADD);
    graph.connect(m->output("Value"), sum->input("Value1"));
    graph.connect(plus, sum->input("Value2"));
    sum->set_use_clamp(clamp);
    return sum->output("Value");
}

/// A colour times a number.
ccl::ShaderOutput* times(ccl::ShaderGraph& graph, ccl::ShaderOutput* color, ccl::ShaderOutput* k) {
    auto* m = graph.create_node<ccl::VectorMathNode>();
    m->set_math_type(ccl::NODE_VECTOR_MATH_SCALE);
    graph.connect(color, m->input("Vector1"));
    graph.connect(k, m->input("Scale"));
    return m->output("Vector");
}

/// A surface the scene has flat, as it is up close: its colour lighter and
/// darker in blotches a metre or two across and in stains a hand wide, its
/// roughness with the stains, bumps `grain` to a unit (40: some 2 cm) -- all
/// as much as `amount` (Settings::detail). `at`: where on it, in its own space.
struct Detail {
    ccl::ShaderOutput* color = nullptr;
    ccl::ShaderOutput* roughness = nullptr;
    ccl::ShaderOutput* normal = nullptr;
};
Detail detailOf(ccl::ShaderGraph& graph, ccl::ShaderOutput* at, ccl::ShaderOutput* color, float roughness, float amount,
                float grain, float bump) {
    Detail d;
    ccl::ShaderOutput* blotches = noise(graph, at, 0.6f, 4.0f, 0.55f);
    ccl::ShaderOutput* stains = noise(graph, at, 6.0f, 5.0f, 0.6f);
    // 1, give or take a fifth.
    ccl::ShaderOutput* lighter = scaled(graph, blotches, 0.45f * amount, 1.0f - 0.225f * amount);
    lighter = scaled(graph, stains, 0.3f * amount, -0.15f * amount, lighter);
    d.color = times(graph, color, lighter);
    d.roughness = scaled(graph, stains, 0.3f * amount, roughness - 0.15f * amount, nullptr, true);
    auto* bumps = graph.create_node<ccl::BumpNode>();
    graph.connect(noise(graph, at, grain, 6.0f, 0.65f), bumps->input("Height"));
    bumps->set_strength(bump * amount);
    bumps->set_distance(0.02f);
    d.normal = bumps->output("Normal");
    return d;
}

/// `at` stretched: times `k`, axis by axis.
ccl::ShaderOutput* stretched(ccl::ShaderGraph& graph, ccl::ShaderOutput* at, const Vec3& k) {
    auto* m = graph.create_node<ccl::VectorMathNode>();
    m->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY);
    graph.connect(at, m->input("Vector1"));
    m->set_vector2(ccl::make_float3(k.x, k.y, k.z));
    return m->output("Vector");
}

/// `at` pushed about by noise `scale` to a unit, up to `amount` units: what
/// makes cells and rings less regular.
ccl::ShaderOutput* warped(ccl::ShaderGraph& graph, ccl::ShaderOutput* at, float scale, float amount) {
    auto* n = graph.create_node<ccl::NoiseTextureNode>();
    n->set_scale(scale);
    n->set_detail(2.0f);
    if (at) graph.connect(at, n->input("Vector"));
    auto* m = graph.create_node<ccl::VectorMathNode>();
    m->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY_ADD);
    graph.connect(n->output("Color"), m->input("Vector1"));
    m->set_vector2(ccl::make_float3(amount, amount, amount));
    if (at) graph.connect(at, m->input("Vector3"));
    return m->output("Vector");
}

/// Voronoi's cells, `scale` to a unit: how far `at` is from the middle of
/// its cell, and from its edge (in cells); a colour of the cell's own.
struct Cells {
    ccl::ShaderOutput* distance;
    ccl::ShaderOutput* edge;
    ccl::ShaderOutput* color;
};
Cells cells(ccl::ShaderGraph& graph, ccl::ShaderOutput* at, float scale) {
    auto* v = graph.create_node<ccl::VoronoiTextureNode>();
    v->set_scale(scale);
    if (at) graph.connect(at, v->input("Vector"));
    auto* e = graph.create_node<ccl::VoronoiTextureNode>();
    e->set_scale(scale);
    e->set_feature(ccl::NODE_VORONOI_DISTANCE_TO_EDGE);
    if (at) graph.connect(at, e->input("Vector"));
    return {v->output("Distance"), e->output("Distance"), v->output("Color")};
}

/// From `lo` where `x` is `from` or less to `hi` where it is `to` or more,
/// smoothly.
ccl::ShaderOutput* ramp(ccl::ShaderGraph& graph, ccl::ShaderOutput* x, float from, float to, float lo = 0.0f,
                        float hi = 1.0f) {
    auto* m = graph.create_node<ccl::MapRangeNode>();
    m->set_range_type(ccl::NODE_MAP_RANGE_SMOOTHSTEP);
    graph.connect(x, m->input("Value"));
    m->set_from_min(from);
    m->set_from_max(to);
    m->set_to_min(lo);
    m->set_to_max(hi);
    m->set_clamp(true);
    return m->output("Result");
}

/// A colour of its own.
ccl::ShaderOutput* constant(ccl::ShaderGraph& graph, const Vec3& c) {
    auto* k = graph.create_node<ccl::ColorNode>();
    k->set_value(rgb(c));
    return k->output("Color");
}

/// `a` and `b` mixed: `fac` of `b`.
ccl::ShaderOutput* mixed(ccl::ShaderGraph& graph, ccl::ShaderOutput* a, ccl::ShaderOutput* b, ccl::ShaderOutput* fac) {
    auto* m = graph.create_node<ccl::MixColorNode>();
    m->set_blend_type(ccl::NODE_MIX_BLEND);
    m->set_use_clamp(true);
    graph.connect(a, m->input("A"));
    graph.connect(b, m->input("B"));
    graph.connect(fac, m->input("Factor"));
    return m->output("Result");
}

/// A colour times a colour.
ccl::ShaderOutput* tinted(ccl::ShaderGraph& graph, ccl::ShaderOutput* color, ccl::ShaderOutput* by) {
    auto* m = graph.create_node<ccl::VectorMathNode>();
    m->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY);
    graph.connect(color, m->input("Vector1"));
    graph.connect(by, m->input("Vector2"));
    return m->output("Vector");
}

/// A MathNode's `type` of `a` and `b` -- or of `a` and `bOut`.
ccl::ShaderOutput* math(ccl::ShaderGraph& graph, ccl::NodeMathType type, ccl::ShaderOutput* a, float b,
                        ccl::ShaderOutput* bOut = nullptr) {
    auto* m = graph.create_node<ccl::MathNode>();
    m->set_math_type(type);
    graph.connect(a, m->input("Value1"));
    m->set_value2(b);
    if (bOut) graph.connect(bOut, m->input("Value2"));
    return m->output("Value");
}

/// How light a colour is.
ccl::ShaderOutput* lightness(ccl::ShaderGraph& graph, ccl::ShaderOutput* color) {
    auto* bw = graph.create_node<ccl::RGBToBWNode>();
    graph.connect(color, bw->input("Color"));
    return bw->output("Val");
}

/// `color` lighter and darker as `n` (0..1, a half on the whole) goes: as
/// much as `k` either way, the same on the whole.
ccl::ShaderOutput* varied(ccl::ShaderGraph& graph, ccl::ShaderOutput* color, ccl::ShaderOutput* n, float k) {
    return times(graph, color, scaled(graph, n, 2.0f * k, 1.0f - k));
}

/// The normal of a surface with bumps as high as `height` -- `distance`
/// units for a height of 1 -- on `normal` (none: the surface's own).
ccl::ShaderOutput* bumped(ccl::ShaderGraph& graph, ccl::ShaderOutput* height, float strength, float distance,
                          ccl::ShaderOutput* normal = nullptr) {
    auto* b = graph.create_node<ccl::BumpNode>();
    graph.connect(height, b->input("Height"));
    if (normal) graph.connect(normal, b->input("Normal"));
    b->set_strength(std::clamp(strength, 0.0f, 1.0f));
    b->set_distance(distance);
    return b->output("Normal");
}

/// How a picture is laid on a surface from three sides (render/Textures.h,
/// as the path tracer does): where on the picture each side looks, and how
/// much of each -- as much as the surface faced that side, to the fourth
/// power. `at` where the surface was before it moved, `face` the way it
/// faced there (pg_rest, pg_rest_normal), `perUnit` pictures to a metre.
struct Laid {
    ccl::ShaderOutput* uv[3];
    ccl::ShaderOutput* weight[3];
};
Laid laidOn(ccl::ShaderGraph& graph, ccl::ShaderOutput* at, ccl::ShaderOutput* face, float perUnit) {
    auto* p = graph.create_node<ccl::SeparateXYZNode>();
    graph.connect(at, p->input("Vector"));
    auto* n = graph.create_node<ccl::SeparateXYZNode>();
    graph.connect(face, n->input("Vector"));
    ccl::ShaderOutput* fourth[3];
    const char* axis[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        ccl::ShaderOutput* square = math(graph, ccl::NODE_MATH_MULTIPLY, n->output(axis[i]), 0.0f, n->output(axis[i]));
        fourth[i] = math(graph, ccl::NODE_MATH_MULTIPLY, square, 0.0f, square);
    }
    ccl::ShaderOutput* sum = math(graph, ccl::NODE_MATH_ADD, math(graph, ccl::NODE_MATH_ADD, fourth[0], 0.0f, fourth[1]), 0.0f,
                                  fourth[2]);
    sum = math(graph, ccl::NODE_MATH_MAXIMUM, sum, 1e-12f);
    Laid laid;
    for (int i = 0; i < 3; ++i) laid.weight[i] = math(graph, ccl::NODE_MATH_DIVIDE, fourth[i], 0.0f, sum);
    // Each side's picture moved, so that the three do not line up.
    auto uv = [&](const char* u, float du, const char* v, float dv) {
        auto* c = graph.create_node<ccl::CombineXYZNode>();
        graph.connect(scaled(graph, p->output(u), perUnit, du), c->input("X"));
        graph.connect(scaled(graph, p->output(v), perUnit, dv), c->input("Y"));
        return c->output("Vector");
    };
    laid.uv[0] = uv("Z", 0.31f, "Y", 0.17f);
    laid.uv[1] = uv("X", 0.53f, "Z", 0.71f);
    laid.uv[2] = uv("X", 0.0f, "Y", 0.0f);
    return laid;
}

/// The picture `file` laid on so: its colour (sRGB) or its value (as it is).
ccl::ShaderOutput* sampled(ccl::ShaderGraph& graph, const Laid& laid, const std::string& file, bool color) {
    ccl::ShaderOutput* sum = nullptr;
    for (int i = 0; i < 3; ++i) {
        auto* image = graph.create_node<ccl::ImageTextureNode>();
        image->set_filename(ccl::ustring(file));
        image->set_colorspace(color ? ccl::u_colorspace_srgb : ccl::u_colorspace_raw);
        graph.connect(laid.uv[i], image->input("Vector"));
        ccl::ShaderOutput* part = color ? times(graph, image->output("Color"), laid.weight[i])
                                        : math(graph, ccl::NODE_MATH_MULTIPLY, image->output("Color"), 0.0f, laid.weight[i]);
        if (!sum) {
            sum = part;
        } else if (color) {
            auto* add = graph.create_node<ccl::VectorMathNode>();
            add->set_math_type(ccl::NODE_VECTOR_MATH_ADD);
            graph.connect(sum, add->input("Vector1"));
            graph.connect(part, add->input("Vector2"));
            sum = add->output("Vector");
        } else {
            sum = math(graph, ccl::NODE_MATH_ADD, sum, 0.0f, part);
        }
    }
    return sum;
}

/// What a material's pattern makes of a surface: its colour, and where it
/// has them its roughness, how metal, its normal, and how strongly it
/// reflects (Principled's Specular IOR Level: a half for most).
struct Pattern {
    ccl::ShaderOutput* color = nullptr;
    ccl::ShaderOutput* roughness = nullptr;
    ccl::ShaderOutput* metallic = nullptr;
    ccl::ShaderOutput* normal = nullptr;
    float specular = 0.5f;
};

/// A photograph laid on a wall goes round and round: over it, what a wall
/// has on the scale of a building -- lighter and darker in patches metres
/// across, dirt run down it in streaks -- as much as `amount`.
ccl::ShaderOutput* weathered(ccl::ShaderGraph& graph, const Material& m, ccl::ShaderOutput* at, ccl::ShaderOutput* color,
                             float amount) {
    const float a = std::clamp(amount, 0.0f, 1.0f);
    if (a <= 0.0f) return color;
    float patches = 0.08f, streaks = 0.0f;
    switch (m.preset) {
        case MaterialPreset::Concrete: streaks = 0.22f; break;
        case MaterialPreset::Plaster: streaks = 0.25f; break;
        case MaterialPreset::BrickWall: streaks = 0.15f; break;
        case MaterialPreset::Soil: patches = 0.15f; break;
        case MaterialPreset::Roof: patches = 0.2f; break;  // puddles dried, tar patched
        default: break;
    }
    ccl::ShaderOutput* c = varied(graph, color, noise(graph, at, 0.3f, 4.0f, 0.55f), patches * a);
    if (streaks > 0.0f) {
        ccl::ShaderOutput* runs = noise(graph, stretched(graph, at, Vec3(4.0f, 0.25f, 4.0f)), 1.5f, 5.0f, 0.6f);
        c = times(graph, c, ramp(graph, runs, 0.5f, 0.78f, 1.0f, 1.0f - streaks * a));
    }
    return c;
}

/// The pattern of `m`'s preset on `color`: `at` where the surface was before
/// it moved, in metres (Y up); `random`, 0..1, a number of the face it is of
/// (windows'); `world` where it is now -- what plants' patches are of, the
/// copies of one plant told apart by it. All of it as much as `amount`
/// (Settings::detail); in three sizes, so that it shows from afar and from
/// near: a building's stains and streaks, a hand's breadth of blotches,
/// grain and pores. Each keeps the colour as it is on the whole.
Pattern patternOf(ccl::ShaderGraph& graph, const Material& m, ccl::ShaderOutput* at, ccl::ShaderOutput* world,
                  ccl::ShaderOutput* random, ccl::ShaderOutput* color, float amount) {
    const float a = std::clamp(amount, 0.0f, 1.0f);
    Pattern p;
    p.color = color;
    // The roughness `spread` either way of the material's as `n` goes.
    auto rough = [&](ccl::ShaderOutput* n, float spread) {
        return scaled(graph, n, 2.0f * spread * a, m.roughness - spread * a, nullptr, true);
    };
    // Dirt run down a wall in streaks: darker by up to `k` in them.
    auto streaked = [&](ccl::ShaderOutput* c, float k) {
        ccl::ShaderOutput* streaks = noise(graph, stretched(graph, at, Vec3(4.0f, 0.25f, 4.0f)), 1.5f, 5.0f, 0.6f);
        return times(graph, c, ramp(graph, streaks, 0.5f, 0.78f, 1.0f, 1.0f - k * a));
    };
    switch (m.preset) {
        case MaterialPreset::None:
        case MaterialPreset::Glass:
            break;
        case MaterialPreset::Concrete: {
            // Cast: lighter and darker in pours a few metres across and in
            // blotches a hand wide, streaked by the rain, its sand, and
            // the holes of the air in it.
            ccl::ShaderOutput* pours = noise(graph, at, 0.3f, 4.0f, 0.55f);
            ccl::ShaderOutput* blotches = noise(graph, at, 4.0f, 6.0f, 0.6f);
            ccl::ShaderOutput* sand = noise(graph, at, 120.0f, 3.0f, 0.6f);
            ccl::ShaderOutput* hole = ramp(graph, cells(graph, warped(graph, at, 30.0f, 0.004f), 30.0f).distance, 0.05f,
                                           0.1f, 1.0f, 0.0f);
            ccl::ShaderOutput* c = varied(graph, color, pours, 0.1f * a);
            c = varied(graph, c, blotches, 0.12f * a);
            c = varied(graph, c, sand, 0.05f * a);
            c = streaked(c, 0.25f);
            p.color = times(graph, c, scaled(graph, hole, -0.5f * a, 1.0f));
            p.roughness = rough(blotches, 0.08f);
            p.normal = bumped(graph, scaled(graph, sand, 0.6f, 0.0f, scaled(graph, hole, -1.0f, 0.0f)), 0.5f * a, 0.0015f);
            break;
        }
        case MaterialPreset::BrokenConcrete: {
            // A break: stones of the aggregate, sharp-edged, each a shade of
            // its own -- greyer, browner, lighter, darker -- in the cement
            // between them, the whole torn rough; darker down in it.
            ccl::ShaderOutput* where = warped(graph, at, 12.0f, 0.012f);
            const Cells stones = cells(graph, where, 22.0f);
            ccl::ShaderOutput* stone = ramp(graph, stones.edge, 0.04f, 0.1f, 0.0f, a);
            ccl::ShaderOutput* kind = lightness(graph, stones.color);
            ccl::ShaderOutput* shade = scaled(graph, kind, 0.8f, 0.6f);  // 0.6..1.4
            ccl::ShaderOutput* warm = mixed(graph, constant(graph, Vec3(0.92f, 0.95f, 1.0f)),
                                            constant(graph, Vec3(1.1f, 1.0f, 0.85f)),
                                            math(graph, ccl::NODE_MATH_FRACTION, scaled(graph, kind, 5.3f, 0.0f), 0.0f));
            ccl::ShaderOutput* rock = tinted(graph, times(graph, color, shade), warm);
            ccl::ShaderOutput* cement = varied(graph, color, noise(graph, at, 60.0f, 3.0f, 0.6f), 0.12f * a);
            ccl::ShaderOutput* torn = noise(graph, at, 9.0f, 8.0f, 0.62f);
            ccl::ShaderOutput* c = mixed(graph, cement, rock, stone);
            // Down in the hollows, the light reaches less.
            p.color = times(graph, c, ramp(graph, torn, 0.25f, 0.7f, 1.0f - 0.45f * a, 1.0f));
            ccl::ShaderOutput* height = scaled(graph, torn, 1.0f, 0.0f, scaled(graph, stone, 0.25f, 0.0f));
            p.normal = bumped(graph, height, 0.8f * a, 0.008f);
            break;
        }
        case MaterialPreset::Brick:
        case MaterialPreset::BrickWall: {
            // Fired clay: each brick blotched lighter and darker, flecked
            // with darker grains, its face pitted.
            ccl::ShaderOutput* blotches = noise(graph, at, 9.0f, 5.0f, 0.6f);
            ccl::ShaderOutput* flecks = ramp(graph, cells(graph, at, 45.0f).distance, 0.07f, 0.12f, 1.0f, 0.0f);
            ccl::ShaderOutput* grain = noise(graph, at, 70.0f, 4.0f, 0.6f);
            ccl::ShaderOutput* c = varied(graph, color, blotches, 0.22f * a);
            c = varied(graph, c, grain, 0.08f * a);
            p.color = times(graph, c, scaled(graph, flecks, -0.45f * a, 1.0f));
            p.roughness = rough(blotches, 0.06f);
            p.normal = bumped(graph, scaled(graph, grain, 1.0f, 0.0f, scaled(graph, flecks, -0.6f, 0.0f)), 0.6f * a, 0.0015f);
            break;
        }
        case MaterialPreset::Mortar: {
            // Sand and lime: grainy, dirtier here and there.
            ccl::ShaderOutput* sand = noise(graph, at, 200.0f, 2.0f, 0.5f);
            ccl::ShaderOutput* dirt = noise(graph, at, 6.0f, 4.0f, 0.55f);
            ccl::ShaderOutput* c = varied(graph, color, sand, 0.15f * a);
            p.color = varied(graph, c, dirt, 0.15f * a);
            p.normal = bumped(graph, sand, 0.6f * a, 0.0012f);
            break;
        }
        case MaterialPreset::Plaster: {
            // Render on a wall: rough-cast, weathered in patches, dirt run
            // down it in streaks.
            ccl::ShaderOutput* cast = noise(graph, at, 50.0f, 8.0f, 0.62f);
            ccl::ShaderOutput* weather = noise(graph, at, 0.6f, 5.0f, 0.6f);
            ccl::ShaderOutput* patches = noise(graph, at, 5.0f, 5.0f, 0.6f);
            ccl::ShaderOutput* c = varied(graph, color, weather, 0.1f * a);
            c = varied(graph, c, patches, 0.06f * a);
            c = varied(graph, c, cast, 0.05f * a);
            p.color = streaked(c, 0.3f);
            p.roughness = rough(weather, 0.08f);
            p.normal = bumped(graph, cast, 0.6f * a, 0.0025f);
            break;
        }
        case MaterialPreset::Window: {
            // Glass with a room behind it: dark, or lighter -- curtains,
            // blinds, a lamp -- warmer or cooler, room by room (`random`:
            // the face's, the object's place added for copies of one).
            auto* info = graph.create_node<ccl::ObjectInfoNode>();
            auto* whose = graph.create_node<ccl::WhiteNoiseTextureNode>();
            graph.connect(info->output("Location"), whose->input("Vector"));
            ccl::ShaderOutput* r = math(graph, ccl::NODE_MATH_FRACTION,
                                        math(graph, ccl::NODE_MATH_ADD, random, 0.0f, whose->output("Value")), 0.0f);
            ccl::ShaderOutput* lit = scaled(graph, math(graph, ccl::NODE_MATH_POWER, r, 3.0f), 1.6f * a, 1.0f - 0.55f * a);
            ccl::ShaderOutput* warmth = math(graph, ccl::NODE_MATH_FRACTION, scaled(graph, r, 7.13f, 0.0f), 0.0f);
            ccl::ShaderOutput* tint = mixed(graph, constant(graph, Vec3(0.85f, 0.93f, 1.05f)),
                                            constant(graph, Vec3(1.12f, 0.95f, 0.75f)), warmth);
            p.color = times(graph, tinted(graph, color, tint), lit);
            // Clean, or not so clean.
            p.roughness = scaled(graph, noise(graph, at, 1.5f, 3.0f, 0.5f), 0.1f * a, m.roughness, nullptr, true);
            p.specular = 0.75f;
            break;
        }
        case MaterialPreset::Steel: {
            // Rebar: dark steel, rusty in places -- rust rough, not metal.
            ccl::ShaderOutput* rust = ramp(graph, noise(graph, at, 9.0f, 5.0f, 0.6f), 0.52f, 0.68f, 0.0f, 0.85f * a);
            ccl::ShaderOutput* rusty = varied(graph, constant(graph, Vec3(0.26f, 0.12f, 0.06f)),
                                              noise(graph, at, 60.0f, 3.0f, 0.5f), 0.25f);
            p.color = mixed(graph, varied(graph, color, noise(graph, at, 30.0f, 3.0f, 0.5f), 0.15f * a), rusty, rust);
            p.metallic = scaled(graph, rust, -m.metallic, m.metallic, nullptr, true);
            p.roughness = scaled(graph, rust, 0.45f, m.roughness, nullptr, true);
            p.normal = bumped(graph, noise(graph, at, 150.0f, 3.0f, 0.5f), 0.4f * a, 0.0008f);
            break;
        }
        case MaterialPreset::Metal: {
            // Sheet metal: smoother and rougher where it was handled, a
            // little lighter and darker.
            ccl::ShaderOutput* wear = noise(graph, at, 4.0f, 4.0f, 0.55f);
            p.color = varied(graph, color, noise(graph, at, 1.0f, 3.0f, 0.5f), 0.06f * a);
            p.roughness = rough(wear, 0.12f);
            p.normal = bumped(graph, noise(graph, at, 300.0f, 2.0f, 0.5f), 0.15f * a, 0.0005f);
            break;
        }
        case MaterialPreset::Asphalt: {
            // Tar and stones a few millimetres across, worn lighter in the
            // tracks, patched darker where it was mended.
            const Cells stones = cells(graph, at, 110.0f);
            ccl::ShaderOutput* stone = ramp(graph, stones.edge, 0.05f, 0.15f, 0.0f, a);
            ccl::ShaderOutput* rock = times(graph, color, scaled(graph, lightness(graph, stones.color), 1.6f, 0.7f));
            ccl::ShaderOutput* c = mixed(graph, color, rock, stone);
            c = varied(graph, c, noise(graph, at, 0.4f, 4.0f, 0.55f), 0.2f * a);
            ccl::ShaderOutput* mended = ramp(graph, noise(graph, at, 0.8f, 3.0f, 0.5f), 0.6f, 0.66f, 1.0f, 1.0f - 0.3f * a);
            c = times(graph, c, mended);
            p.color = varied(graph, c, noise(graph, at, 3.0f, 4.0f, 0.55f), 0.14f * a);
            p.normal = bumped(graph, stone, 0.5f * a, 0.0012f);
            break;
        }
        case MaterialPreset::Wood: {
            // The grain: rings, wavy, fine fibres along them.
            auto* wave = graph.create_node<ccl::WaveTextureNode>();
            wave->set_wave_type(ccl::NODE_WAVE_RINGS);
            wave->set_rings_direction(ccl::NODE_WAVE_RINGS_DIRECTION_Y);
            wave->set_scale(3.0f);
            wave->set_distortion(4.0f);
            wave->set_detail(4.0f);
            wave->set_detail_scale(1.2f);
            graph.connect(stretched(graph, at, Vec3(1.0f, 0.15f, 1.0f)), wave->input("Vector"));
            ccl::ShaderOutput* fibres = noise(graph, stretched(graph, at, Vec3(60.0f, 1.0f, 60.0f)), 1.0f, 4.0f, 0.6f);
            ccl::ShaderOutput* c = varied(graph, color, wave->output("Fac"), 0.18f * a);
            p.color = varied(graph, c, fibres, 0.1f * a);
            p.roughness = rough(fibres, 0.1f);
            p.normal = bumped(graph, fibres, 0.3f * a, 0.0008f);
            break;
        }
        case MaterialPreset::Stone: {
            // Grains of a few colours, larger patches, uneven.
            const Cells grains = cells(graph, at, 60.0f);
            ccl::ShaderOutput* c = varied(graph, color, lightness(graph, grains.color), 0.15f * a);
            c = varied(graph, c, noise(graph, at, 3.0f, 5.0f, 0.6f), 0.14f * a);
            p.color = varied(graph, c, noise(graph, at, 0.4f, 4.0f, 0.55f), 0.1f * a);
            p.normal = bumped(graph, noise(graph, at, 18.0f, 6.0f, 0.6f), 0.5f * a, 0.003f);
            break;
        }
        case MaterialPreset::Roof: {
            // A flat roof: a membrane under fine gravel, stained by water.
            const Cells gravel = cells(graph, at, 70.0f);
            ccl::ShaderOutput* c = varied(graph, color, lightness(graph, gravel.color), 0.22f * a);
            c = varied(graph, c, noise(graph, at, 0.4f, 4.0f, 0.55f), 0.15f * a);
            p.color = varied(graph, c, noise(graph, at, 4.0f, 4.0f, 0.55f), 0.1f * a);
            p.normal = bumped(graph, ramp(graph, gravel.edge, 0.0f, 0.2f), 0.5f * a, 0.0015f);
            break;
        }
        case MaterialPreset::Bark: {
            // Furrows up the trunk: darker in them, deep.
            ccl::ShaderOutput* furrows = noise(graph, stretched(graph, at, Vec3(8.0f, 1.0f, 8.0f)), 2.5f, 6.0f, 0.6f);
            ccl::ShaderOutput* ridge = ramp(graph, furrows, 0.35f, 0.6f);
            p.color = times(graph, color, scaled(graph, ridge, 0.5f * a, 1.0f - 0.35f * a));
            p.normal = bumped(graph, ridge, 0.9f * a, 0.01f);
            break;
        }
        case MaterialPreset::Leaf: {
            // Some leaves yellower, some darker -- in patches over the tree.
            ccl::ShaderOutput* patch = noise(graph, world, 0.8f, 2.0f, 0.5f);
            ccl::ShaderOutput* yellow = tinted(graph, color, constant(graph, Vec3(1.25f, 1.1f, 0.55f)));
            p.color = varied(graph, mixed(graph, color, yellow, ramp(graph, patch, 0.55f, 0.8f, 0.0f, 0.5f * a)),
                             noise(graph, world, 3.0f, 2.0f, 0.5f), 0.12f * a);
            break;
        }
        case MaterialPreset::Grass: {
            // Drier in patches a few metres across.
            ccl::ShaderOutput* patch = noise(graph, world, 0.25f, 3.0f, 0.5f);
            ccl::ShaderOutput* dry = times(graph, constant(graph, Vec3(1.45f, 1.2f, 0.55f)),
                                           scaled(graph, lightness(graph, color), 0.85f, 0.0f));
            ccl::ShaderOutput* c = mixed(graph, color, dry, ramp(graph, patch, 0.5f, 0.75f, 0.0f, 0.65f * a));
            p.color = varied(graph, c, noise(graph, world, 2.0f, 2.0f, 0.5f), 0.1f * a);
            break;
        }
        case MaterialPreset::Soil: {
            // Crumbs and clods, darker where it is damp.
            const Cells clods = cells(graph, warped(graph, at, 8.0f, 0.02f), 14.0f);
            ccl::ShaderOutput* crumbs = noise(graph, at, 40.0f, 4.0f, 0.65f);
            ccl::ShaderOutput* damp = noise(graph, at, 0.8f, 4.0f, 0.55f);
            ccl::ShaderOutput* c = varied(graph, color, lightness(graph, clods.color), 0.15f * a);
            c = varied(graph, c, crumbs, 0.12f * a);
            p.color = times(graph, c, ramp(graph, damp, 0.5f, 0.75f, 1.0f, 1.0f - 0.35f * a));
            p.normal = bumped(graph, scaled(graph, ramp(graph, clods.edge, 0.0f, 0.25f), 0.6f, 0.0f, crumbs), 0.6f * a, 0.004f);
            break;
        }
    }
    return p;
}

/// How bright Nishita's sun is at `elevation` radians, `size` radians across:
/// its radiance times the square of its width (Cycles' own estimate), a
/// quarter of pi of which is the light it sheds.
float nishitaSun(float elevation, float size) {
    ccl::SkyTextureNode probe;
    probe.set_sky_type(ccl::NODE_SKY_NISHITA);
    probe.set_sun_elevation(elevation);
    probe.set_sun_size(size);
    return probe.get_sun_average_radiance();
}

/// The colour of Nishita's sun `elevation` radians up: linear Rec. 709 --
/// Cycles' -- of luminance 1.
Vec3 nishitaSunColour(float elevation, float size) {
    float bottom[3], top[3];
    SKY_nishita_skymodel_precompute_sun(elevation, size, 1.0f, 1.0f, 1.0f, bottom, top);
    const float x = bottom[0] + top[0], y = bottom[1] + top[1], z = bottom[2] + top[2];
    const Vec3 c(3.2406f * x - 1.5372f * y - 0.4986f * z, -0.9689f * x + 1.8758f * y + 0.0415f * z,
                 0.0557f * x - 0.2040f * y + 1.0570f * z);
    const float luminance = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    return luminance > 0.0f ? c / luminance : Vec3(1.0f, 1.0f, 1.0f);
}

/// A sphere, box, cylinder, cone or torus in triangles fine enough not to
/// show them, its normals the shape's own; a mesh object, its mesh.
void tessellate(const sim::ShapeInstance& s, std::vector<Vec3>& points, std::vector<Vec3>& normals) {
    auto tri = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
        const Vec3 wa = s.toWorld(a), wb = s.toWorld(b), wc = s.toWorld(c);
        points.insert(points.end(), {wa, wb, wc});
        normals.insert(normals.end(), {s.normal(wa), s.normal(wb), s.normal(wc)});
    };
    const Vec3 h = s.half();
    constexpr int kAround = 64, kAlong = 32;
    auto ring = [&](float a) { return Vec3(std::cos(a), 0.0f, std::sin(a)); };
    switch (s.shape()) {
        case sim::Shape::Sphere:
            for (int j = 0; j < kAlong; ++j) {
                const float t0 = kPi * static_cast<float>(j) / kAlong, t1 = kPi * static_cast<float>(j + 1) / kAlong;
                for (int i = 0; i < kAround; ++i) {
                    const float a0 = 2.0f * kPi * static_cast<float>(i) / kAround, a1 = 2.0f * kPi * static_cast<float>(i + 1) / kAround;
                    auto at = [&](float t, float a) {
                        return Vec3(std::sin(t) * std::cos(a) * h.x, std::cos(t) * h.y, std::sin(t) * std::sin(a) * h.z);
                    };
                    tri(at(t0, a0), at(t1, a0), at(t1, a1));
                    tri(at(t0, a0), at(t1, a1), at(t0, a1));
                }
            }
            break;
        case sim::Shape::Cylinder:
        case sim::Shape::Cone: {
            const bool cone = s.shape() == sim::Shape::Cone;
            for (int i = 0; i < kAround; ++i) {
                const Vec3 r0 = ring(2.0f * kPi * static_cast<float>(i) / kAround), r1 = ring(2.0f * kPi * static_cast<float>(i + 1) / kAround);
                const Vec3 b0(r0.x * h.x, -h.y, r0.z * h.z), b1(r1.x * h.x, -h.y, r1.z * h.z);
                const Vec3 t0 = cone ? Vec3(0.0f, h.y, 0.0f) : Vec3(b0.x, h.y, b0.z);
                const Vec3 t1 = cone ? Vec3(0.0f, h.y, 0.0f) : Vec3(b1.x, h.y, b1.z);
                tri(b0, t0, t1);
                if (!cone) tri(b0, t1, b1);
                tri(Vec3(0.0f, -h.y, 0.0f), b1, b0);
                if (!cone) tri(Vec3(0.0f, h.y, 0.0f), t0, t1);
            }
            break;
        }
        case sim::Shape::Torus: {
            const float k = h.z / h.x;
            for (int j = 0; j < kAlong; ++j) {
                for (int i = 0; i < kAround; ++i) {
                    auto at = [&](int ii, int jj) {
                        const float a = 2.0f * kPi * static_cast<float>(ii) / kAround, b = 2.0f * kPi * static_cast<float>(jj) / kAlong;
                        const float r = s.ring() + s.tube() * std::cos(b);
                        return Vec3(r * std::cos(a), s.tube() * std::sin(b), r * std::sin(a) * k);
                    };
                    tri(at(i, j), at(i + 1, j), at(i + 1, j + 1));
                    tri(at(i, j), at(i + 1, j + 1), at(i, j + 1));
                }
            }
            break;
        }
        case sim::Shape::Mesh:
            if (const sim::MeshShape* m = s.mesh()) {
                // Mesh space back to the shape's own axes: toMesh is linear.
                const Vec3 o = s.toMesh(Vec3());
                const Vec3 k(s.toMesh(Vec3(1.0f, 0.0f, 0.0f)).x - o.x, s.toMesh(Vec3(0.0f, 1.0f, 0.0f)).y - o.y,
                             s.toMesh(Vec3(0.0f, 0.0f, 1.0f)).z - o.z);
                const auto& mesh = m->mesh();
                for (const auto& t : mesh.triangles) {
                    Vec3 p[3];
                    for (int c = 0; c < 3; ++c) {
                        const Vec3& q = mesh.positions[t[static_cast<size_t>(c)]];
                        p[c] = Vec3((q.x - o.x) / k.x, (q.y - o.y) / k.y, (q.z - o.z) / k.z);
                    }
                    const Vec3 wa = s.toWorld(p[0]), wb = s.toWorld(p[1]), wc = s.toWorld(p[2]);
                    Vec3 n = cross(wb - wa, wc - wa);
                    const float len = length(n);
                    n = len > 0.0f ? n / len : Vec3(0.0f, 1.0f, 0.0f);
                    points.insert(points.end(), {wa, wb, wc});
                    normals.insert(normals.end(), {n, n, n});
                }
                break;
            }
            [[fallthrough]];
        case sim::Shape::Box: {
            static const int kFaces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
            auto corner = [&](int c) { return Vec3(c & 1 ? h.x : -h.x, c & 2 ? h.y : -h.y, c & 4 ? h.z : -h.z); };
            for (const auto& f : kFaces) {
                const Vec3 a = corner(f[0]), b = corner(f[1]), c = corner(f[2]), d = corner(f[3]);
                const Vec3 n = normalize(s.turn().apply(normalize(cross(b - a, c - a))));
                for (const Vec3* q : {&a, &b, &c, &a, &c, &d}) {
                    points.push_back(s.toWorld(*q));
                    normals.push_back(n);
                }
            }
            break;
        }
    }
}

}  // namespace

struct CyclesRender::Impl {
    bool interactive = false;
    std::unique_ptr<ccl::Session> session;
    ccl::SessionParams params;
    bool started = false;
    Pictures pictures;
    Settings settings;

    // What the last scenes made, kept for the next: our meshes' Cycles
    // meshes, the materials' shaders.
    struct KeptMesh {
        std::weak_ptr<const Mesh> mesh;
        ccl::Mesh* cycles = nullptr;
    };
    std::map<const Mesh*, KeptMesh> meshes;
    std::map<std::tuple<int, std::vector<float>, std::string>, ccl::Shader*> shaders;
    std::vector<ccl::Object*> objects;   // this scene's
    std::vector<ccl::Geometry*> owned;   // its own geometry: the floor, the objects' meshes, the sun
    ccl::Shader* sunShader = nullptr;
    uint64_t imageId = 0;  // the pictures Cycles is given, told apart
    // The light water gives off as ours does, in the scene synced now.
    Vec3 waterGlow;
    ccl::Shader* gasShader = nullptr;

    void make() {
        params = ccl::SessionParams();
        const ccl::vector<ccl::DeviceInfo> devices = ccl::Device::available_devices(ccl::DEVICE_MASK_CPU);
        if (!devices.empty()) params.device = devices.front();
        params.background = !interactive;
        params.threads = 0;  // all the cores
        params.use_resolution_divider = interactive;
        params.use_auto_tile = !interactive;
        ccl::SceneParams sceneParams;
        sceneParams.shadingsystem = ccl::SHADINGSYSTEM_SVM;
        sceneParams.bvh_type = interactive ? ccl::BVH_TYPE_DYNAMIC : ccl::BVH_TYPE_STATIC;
        session = std::make_unique<ccl::Session>(params, sceneParams);
        session->set_output_driver(std::make_unique<Output>(pictures));
        if (interactive) session->set_display_driver(std::make_unique<Display>(pictures));
    }

    /// The shader of a material: a Principled BSDF of the colour of the
    /// surface -- its attribute "Col" times its object's colour (an
    /// instance's tint) -- a translucent one mixed in by the translucency,
    /// with the pattern of what it is made of (patternOf) or else the detail
    /// of a real surface (Settings::detail); glass; water, bending light and
    /// taking on the Water Look's colour.
    ccl::Shader* shaderOf(ccl::Scene* scene, const Material& m, const sim::Look& look) {
        const float detail = m.kind == Material::Kind::Surface ? std::clamp(settings.detail, 0.0f, 1.0f) : 0.0f;
        const TextureSet texture = textureOf(m, settings);
        std::vector<float> key = {m.roughness, m.metallic, m.translucency, m.ior, detail, static_cast<float>(m.preset)};
        if (m.kind == Material::Kind::Water) {
            key.insert(key.end(), {waterGlow.x, waterGlow.y, waterGlow.z, look.waterClarity});
        }
        if (texture.valid()) {
            key.insert(key.end(), {texture.size, texture.depth, texture.mean.x, texture.mean.y, texture.mean.z,
                                   texture.tint ? 1.0f : 0.0f});
        }
        const auto k = std::make_tuple(static_cast<int>(m.kind), key,
                                       texture.color + '|' + texture.height + '|' + texture.roughness);
        if (auto it = shaders.find(k); it != shaders.end()) return it->second;
        auto graph = std::make_unique<ccl::ShaderGraph>();
        auto* attr = graph->create_node<ccl::AttributeNode>();
        attr->set_attribute(ccl::ustring("Col"));
        auto* info = graph->create_node<ccl::ObjectInfoNode>();
        auto* tint = graph->create_node<ccl::VectorMathNode>();
        tint->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY);
        graph->connect(attr->output("Color"), tint->input("Vector1"));
        graph->connect(info->output("Color"), tint->input("Vector2"));
        ccl::ShaderOutput* color = tint->output("Vector");
        ccl::ShaderOutput* surface = nullptr;
        auto* shader = scene->create_node<ccl::Shader>();
        switch (m.kind) {
            case Material::Kind::Surface: {
                auto* bsdf = principled(*graph);
                bsdf->set_roughness(m.roughness);
                bsdf->set_metallic(m.metallic);
                if (texture.valid()) {
                    // Its photographs (render/Textures.h), laid on by where it
                    // was before it moved: the colour -- round Cd, or as it
                    // is -- bumps of its height, its roughness.
                    auto* rest = graph->create_node<ccl::AttributeNode>();
                    rest->set_attribute(ccl::ustring("pg_rest"));
                    auto* face = graph->create_node<ccl::AttributeNode>();
                    face->set_attribute(ccl::ustring("pg_rest_normal"));
                    const Laid laid = laidOn(*graph, rest->output("Vector"), face->output("Vector"), 1.0f / texture.size);
                    ccl::ShaderOutput* picture = sampled(*graph, laid, texture.color, true);
                    if (texture.tint) {
                        const Vec3 mean = glm::max(texture.mean, Vec3(1e-4f));
                        color = tinted(*graph, color, tinted(*graph, picture, constant(*graph, Vec3(1.0f) / mean)));
                    } else {
                        color = tinted(*graph, picture, info->output("Color"));
                    }
                    // Weathered on the scale of a building, where it is a wall.
                    color = weathered(*graph, m, rest->output("Vector"), color, detail);
                    auto* most = graph->create_node<ccl::VectorMathNode>();
                    most->set_math_type(ccl::NODE_VECTOR_MATH_MINIMUM);
                    graph->connect(color, most->input("Vector1"));
                    most->set_vector2(ccl::make_float3(0.95f, 0.95f, 0.95f));
                    color = most->output("Vector");
                    ccl::ShaderOutput* height = nullptr;
                    if (!texture.height.empty()) {
                        height = sampled(*graph, laid, texture.height, false);
                        graph->connect(bumped(*graph, height, 1.0f, texture.depth), bsdf->input("Normal"));
                    }
                    if (!texture.roughness.empty()) {
                        graph->connect(sampled(*graph, laid, texture.roughness, false), bsdf->input("Roughness"));
                    } else if (height) {
                        // Down in it, rougher; on top, worn smoother.
                        graph->connect(scaled(*graph, height, -0.2f, m.roughness + 0.1f, nullptr, true), bsdf->input("Roughness"));
                    }
                } else if (detail > 0.0f && m.preset != MaterialPreset::None) {
                    // Where it was before it moved (meshOf: pg_rest), a
                    // number of its face (pg_random), where it is.
                    auto* rest = graph->create_node<ccl::AttributeNode>();
                    rest->set_attribute(ccl::ustring("pg_rest"));
                    auto* random = graph->create_node<ccl::AttributeNode>();
                    random->set_attribute(ccl::ustring("pg_random"));
                    auto* where = graph->create_node<ccl::GeometryNode>();
                    const Pattern p = patternOf(*graph, m, rest->output("Vector"), where->output("Position"),
                                                random->output("Fac"), color, detail);
                    color = p.color;
                    if (p.roughness) graph->connect(p.roughness, bsdf->input("Roughness"));
                    if (p.metallic) graph->connect(p.metallic, bsdf->input("Metallic"));
                    if (p.normal) graph->connect(p.normal, bsdf->input("Normal"));
                    bsdf->set_specular_ior_level(p.specular);
                } else if (detail > 0.0f) {
                    // Leaves and blades are thin: their bumps small.
                    auto* where = graph->create_node<ccl::TextureCoordinateNode>();
                    const Detail d = detailOf(*graph, where->output("Object"), color, m.roughness, detail, 40.0f,
                                              m.translucency > 0.0f ? 0.1f : 0.35f);
                    color = d.color;
                    graph->connect(d.roughness, bsdf->input("Roughness"));
                    graph->connect(d.normal, bsdf->input("Normal"));
                }
                graph->connect(color, bsdf->input("Base Color"));
                surface = bsdf->output("BSDF");
                if (m.translucency > 0.0f) {
                    auto* through = graph->create_node<ccl::TranslucentBsdfNode>();
                    graph->connect(color, through->input("Color"));
                    auto* mix = graph->create_node<ccl::MixClosureNode>();
                    mix->set_fac(std::clamp(m.translucency, 0.0f, 1.0f));
                    graph->connect(surface, mix->input("Closure1"));
                    graph->connect(through->output("BSDF"), mix->input("Closure2"));
                    surface = mix->output("Closure");
                }
                break;
            }
            case Material::Kind::Glass: {
                // Tinted as ours is: a little of the colour at each face.
                auto* tinted = graph->create_node<ccl::VectorMathNode>();
                tinted->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY_ADD);
                graph->connect(color, tinted->input("Vector1"));
                tinted->set_vector2(ccl::make_float3(0.35f, 0.35f, 0.35f));
                tinted->set_vector3(ccl::make_float3(0.65f, 0.65f, 0.65f));
                auto* glass = graph->create_node<ccl::GlassBsdfNode>();
                glass->set_IOR(m.ior);
                glass->set_roughness(0.0f);
                graph->connect(tinted->output("Vector"), glass->input("Color"));
                // The sun through it, as through ours, a little less of it
                // -- not only the caustics Cycles finds.
                auto* shade = graph->create_node<ccl::VectorMathNode>();
                shade->set_math_type(ccl::NODE_VECTOR_MATH_SCALE);
                graph->connect(tinted->output("Vector"), shade->input("Vector1"));
                shade->set_scale(0.92f);
                surface = throughForShadows(*graph, glass->output("BSDF"), shade->output("Vector"));
                break;
            }
            case Material::Kind::Water: {
                auto* glass = graph->create_node<ccl::GlassBsdfNode>();
                glass->set_IOR(m.ior);
                glass->set_roughness(0.0f);
                glass->set_color(ccl::one_float3());
                surface = throughForShadows(*graph, glass->output("BSDF"), nullptr, 0.9f);
                // Inside, as ours: as much of the light lost as Clarity
                // says, the water's own colour coming in instead -- the
                // deeper, the more of it; the sun's way down left to the
                // faces, as ours leaves it.
                const float density = 1.0f / std::max(look.waterClarity, 1e-3f);
                auto* lost = graph->create_node<ccl::AbsorptionVolumeNode>();
                lost->set_color(ccl::zero_float3());
                lost->set_density(density);
                auto* glow = graph->create_node<ccl::EmissionNode>();
                glow->set_color(rgb(waterGlow));
                glow->set_strength(density);
                auto* both = graph->create_node<ccl::AddClosureNode>();
                graph->connect(lost->output("Volume"), both->input("Closure1"));
                graph->connect(glow->output("Emission"), both->input("Closure2"));
                auto* path = graph->create_node<ccl::LightPathNode>();
                auto* notForShadows = graph->create_node<ccl::MixClosureNode>();
                graph->connect(path->output("Is Shadow Ray"), notForShadows->input("Fac"));
                graph->connect(both->output("Closure"), notForShadows->input("Closure1"));
                graph->connect(notForShadows->output("Closure"), graph->output()->input("Volume"));
                break;
            }
        }
        graph->connect(surface, graph->output()->input("Surface"));
        shader->set_graph(std::move(graph));
        shader->tag_update(scene);
        shaders[k] = shader;
        return shader;
    }

    /// The Cycles mesh of ours: its triangles one by one, the normals and
    /// the colours of their corners; made once while ours lives.
    ccl::Mesh* meshOf(ccl::Scene* scene, const std::shared_ptr<const Mesh>& m, const sim::Look& look) {
        // The shaders of its materials, as the settings have them now.
        auto shadersOf = [&] {
            ccl::array<ccl::Node*> used;
            for (const Material& mat : m->materials) used.push_back_slow(shaderOf(scene, mat, look));
            if (used.empty()) used.push_back_slow(shaderOf(scene, Material(), look));
            return used;
        };
        if (auto it = meshes.find(m.get()); it != meshes.end()) {
            if (it->second.mesh.lock() == m) {
                // Kept -- but Surface Detail or Textures may have changed
                // what its materials are drawn with.
                ccl::Mesh* kept = it->second.cycles;
                ccl::array<ccl::Node*> used = shadersOf();
                if (!(used == kept->get_used_shaders())) kept->set_used_shaders(used);
                return kept;
            }
            scene->delete_node(it->second.cycles);
            meshes.erase(it);
        }
        auto* mesh = scene->create_node<ccl::Mesh>();
        const size_t n = m->count();
        ccl::array<ccl::float3> verts;
        verts.resize(3 * n);
        for (size_t t = 0; t < n; ++t) {
            const Vec3 a = m->v0[t], b = a + m->e1[t], c = a + m->e2[t];
            verts[3 * t] = ccl::make_float3(a.x, a.y, a.z);
            verts[3 * t + 1] = ccl::make_float3(b.x, b.y, b.z);
            verts[3 * t + 2] = ccl::make_float3(c.x, c.y, c.z);
        }
        ccl::array<ccl::Node*> used = shadersOf();
        // Counted before Cycles takes them: setting a node's array swaps it.
        const int kinds = static_cast<int>(used.size());
        mesh->set_used_shaders(used);
        mesh->reserve_mesh(3 * n, n);
        mesh->set_verts(verts);
        for (size_t t = 0; t < n; ++t) {
            const int i = static_cast<int>(3 * t);
            const int shader = t < m->material.size() ? std::min<int>(m->material[t], kinds - 1) : 0;
            mesh->add_triangle(i, i + 1, i + 2, shader, true);
        }
        ccl::float3* normals = mesh->attributes.add(ccl::ATTR_STD_VERTEX_NORMAL)->data_float3();
        ccl::float3* colors = mesh->attributes.add(ccl::ustring("Col"), ccl::TypeColor, ccl::ATTR_ELEMENT_VERTEX)->data_float3();
        for (size_t i = 0; i < 3 * n; ++i) {
            const Vec3 nn = i < m->normals.size() ? m->normals[i] : Vec3(0.0f, 1.0f, 0.0f);
            const Vec3 cc = i < m->colors.size() ? m->colors[i] : Vec3(0.8f, 0.8f, 0.8f);
            normals[i] = ccl::make_float3(nn.x, nn.y, nn.z);
            colors[i] = ccl::make_float3(cc.x, cc.y, cc.z);
        }
        // What a material's pattern is drawn by (patternOf): where the
        // corners were before they moved -- where they are, for what never
        // moved -- ours, in metres; and the windows' numbers.
        const bool patterned = std::any_of(m->materials.begin(), m->materials.end(), [](const Material& mat) {
            return mat.preset != MaterialPreset::None || !mat.texture.empty();
        });
        if (patterned) {
            ccl::float3* rest = mesh->attributes.add(ccl::ustring("pg_rest"), ccl::TypePoint, ccl::ATTR_ELEMENT_VERTEX)->data_float3();
            // ... and the way each face faced there, which way a picture is
            // laid on it from (laidOn).
            ccl::float3* faced =
                mesh->attributes.add(ccl::ustring("pg_rest_normal"), ccl::TypeNormal, ccl::ATTR_ELEMENT_FACE)->data_float3();
            const bool moved = m->rest.size() == 3 * n;
            for (size_t t = 0; t < n; ++t) {
                const Vec3 corner[3] = {m->v0[t], m->v0[t] + m->e1[t], m->v0[t] + m->e2[t]};
                Vec3 r[3];
                for (size_t c = 0; c < 3; ++c) {
                    r[c] = moved ? m->rest[3 * t + c] : corner[c];
                    rest[3 * t + c] = ccl::make_float3(r[c].x, r[c].y, r[c].z);
                }
                const Vec3 across = cross(r[1] - r[0], r[2] - r[0]);
                const float l = length(across);
                const Vec3 f = l > 0.0f ? across / l : Vec3(0.0f, 1.0f, 0.0f);
                faced[t] = ccl::make_float3(f.x, f.y, f.z);
            }
        }
        if (m->random.size() == n) {
            float* random = mesh->attributes.add(ccl::ustring("pg_random"), ccl::TypeFloat, ccl::ATTR_ELEMENT_FACE)->data_float();
            std::copy(m->random.begin(), m->random.end(), random);
        }
        meshes[m.get()] = {m, mesh};
        return mesh;
    }

    /// A mesh of world points (the floor, an object of the scene), made for
    /// this scene alone; `colour`, the colour of all of it.
    ccl::Mesh* ownMesh(ccl::Scene* scene, const std::vector<Vec3>& points, const std::vector<Vec3>& normals,
                       const Vec3& colour, ccl::Shader* shader) {
        auto* mesh = scene->create_node<ccl::Mesh>();
        const size_t n = points.size() / 3;
        ccl::array<ccl::float3> verts;
        verts.resize(points.size());
        for (size_t i = 0; i < points.size(); ++i) verts[i] = ccl::make_float3(points[i].x, points[i].y, points[i].z);
        ccl::array<ccl::Node*> used;
        used.push_back_slow(shader);
        mesh->set_used_shaders(used);
        mesh->reserve_mesh(points.size(), n);
        mesh->set_verts(verts);
        for (size_t t = 0; t < n; ++t) {
            // Turned the way its normals point: Cycles takes the side it
            // is seen from by the order of the corners, and shades a
            // surface whose normals point the other way black.
            const int i = static_cast<int>(3 * t);
            const Vec3 face = cross(points[3 * t + 1] - points[3 * t], points[3 * t + 2] - points[3 * t]);
            const bool turned = dot(face, normals[3 * t] + normals[3 * t + 1] + normals[3 * t + 2]) < 0.0f;
            mesh->add_triangle(i, turned ? i + 2 : i + 1, turned ? i + 1 : i + 2, 0, true);
        }
        ccl::float3* nn = mesh->attributes.add(ccl::ATTR_STD_VERTEX_NORMAL)->data_float3();
        ccl::float3* cc = mesh->attributes.add(ccl::ustring("Col"), ccl::TypeColor, ccl::ATTR_ELEMENT_VERTEX)->data_float3();
        for (size_t i = 0; i < points.size(); ++i) {
            nn[i] = ccl::make_float3(normals[i].x, normals[i].y, normals[i].z);
            cc[i] = rgb(colour);
        }
        owned.push_back(mesh);
        return mesh;
    }

    /// The sun's elevation, radians -- for the sky's brightness and colour no
    /// lower than 3 degrees -- and its width.
    float sunElevation(const Scene& s) const {
        return std::asin(std::clamp(ccl::normalize(toCycles(s.sunDirection)).z, -1.0f, 1.0f));
    }
    float sunSize() const { return std::clamp(settings.sunAngle, 0.01f, 30.0f) * kPi / 180.0f; }

    /// Nishita's sky in `graph`, as Blender's Sky Texture -- its sun where
    /// the look's is, as wide as Sun Size -- its colours made such that its
    /// sun is the look's (Light Color): the sky as blue as it is beside
    /// that sun. `disc`: with the sun's disc. `at`: the way it is looked at,
    /// else the ray's.
    ccl::ShaderOutput* daySky(ccl::ShaderGraph& graph, const Scene& s, bool disc = true,
                              ccl::ShaderOutput* at = nullptr) const {
        auto* day = graph.create_node<ccl::SkyTextureNode>();
        day->set_sky_type(ccl::NODE_SKY_NISHITA);
        const ccl::float3 d = ccl::normalize(toCycles(s.sunDirection));
        day->set_sun_elevation(sunElevation(s));
        day->set_sun_rotation(std::atan2(d.x, d.y));
        day->set_sun_size(sunSize());
        day->set_sun_disc(disc);
        if (at) graph.connect(at, day->input("Vector"));
        const Vec3 model = nishitaSunColour(std::max(sunElevation(s), 3.0f * kPi / 180.0f), sunSize());
        const float lum = 0.2126f * s.sunLight.x + 0.7152f * s.sunLight.y + 0.0722f * s.sunLight.z;
        const Vec3 wanted = lum > 0.0f ? s.sunLight / lum : Vec3(1.0f, 1.0f, 1.0f);
        auto* tinted = graph.create_node<ccl::VectorMathNode>();
        tinted->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY);
        graph.connect(day->output("Color"), tinted->input("Vector1"));
        tinted->set_vector2(ccl::make_float3(wanted.x / model.x, wanted.y / model.y, wanted.z / model.z));
        return tinted->output("Vector");
    }
    /// How strong it is: its sun as bright as the look's. Cycles' estimate
    /// of the sun's light (Nishita's sun) misses some -- its limb is darker,
    /// its light measured as X, Y and Z on the whole: 1.15 times it is what
    /// it sheds (render_cycles_lights_a_day_under_a_physical_sky).
    float dayStrength(const Scene& s) const {
        const float sun = 0.2126f * s.sunLight.x + 0.7152f * s.sunLight.y + 0.0722f * s.sunLight.z;
        return 1.15f * 4.0f * sun /
               std::max(nishitaSun(std::max(sunElevation(s), 3.0f * kPi / 180.0f), sunSize()), 1e-6f);
    }

    /// What the sky is, as asked and as it can be: a picture, when there is
    /// one -- else Nishita's, when there is a sun to make it as bright as --
    /// else the look's.
    Settings::Sky skyKind(const Scene& s) const {
        std::error_code ec;
        if (settings.sky == Settings::Sky::Image && !settings.skyImage.empty() &&
            std::filesystem::is_regular_file(settings.skyImage, ec)) {
            return Settings::Sky::Image;
        }
        const float sun = 0.2126f * s.sunLight.x + 0.7152f * s.sunLight.y + 0.0722f * s.sunLight.z;
        return settings.sky != Settings::Sky::Look && sun > 0.0f ? Settings::Sky::Physical : Settings::Sky::Look;
    }
    bool physicalSky(const Scene& s) const { return skyKind(s) == Settings::Sky::Physical; }

    /// The sky picture all round, turned as Sky Rotation says; looked up
    /// the way `at` gives, else the ray's.
    ccl::ShaderOutput* imageSky(ccl::ShaderGraph& graph, ccl::ShaderOutput* at = nullptr) const {
        auto* env = graph.create_node<ccl::EnvironmentTextureNode>();
        env->set_filename(ccl::ustring(settings.skyImage));
        env->set_projection(ccl::NODE_ENVIRONMENT_EQUIRECTANGULAR);
        env->set_interpolation(ccl::INTERPOLATION_LINEAR);
        env->tex_mapping.rotation = ccl::make_float3(0.0f, 0.0f, settings.skyRotation * kPi / 180.0f);
        if (at) graph.connect(at, env->input("Vector"));
        return env->output("Color");
    }

    /// The sky's light from along `at` (the ray's way without it), before
    /// skyStrength(): a picture's, else Nishita's -- without its sun's disc,
    /// the look's sun lighting (sun()) -- with its clouds unless `clear`.
    ccl::ShaderOutput* skyAt(ccl::ShaderGraph& graph, const Scene& s, ccl::ShaderOutput* at, bool clear = false) const {
        if (skyKind(s) == Settings::Sky::Image) return imageSky(graph, at);
        ccl::ShaderOutput* sky = daySky(graph, s, false, at);
        if (clear || settings.clouds <= 0.0f) return sky;
        if (!at) at = graph.create_node<ccl::TextureCoordinateNode>()->output("Generated");
        return clouded(graph, s, sky, at);
    }
    float skyStrength(const Scene& s) const {
        return skyKind(s) == Settings::Sky::Image ? settings.skyStrength : dayStrength(s);
    }

    /// Clouds over `sky` along `dir`: a layer 2 km up, its holes where
    /// Perlin's noise -- Cloud Size kilometres or so across -- is below what
    /// Clouds asks, drifting on the wind; lit by the sun, brighter round it,
    /// greyer where they are thick and the more of the sky they cover;
    /// thinning into the haze at the horizon.
    ccl::ShaderOutput* clouded(ccl::ShaderGraph& graph, const Scene& s, ccl::ShaderOutput* sky, ccl::ShaderOutput* dir) const {
        const float cover = std::clamp(settings.clouds, 0.0f, 1.0f);
        auto* split = graph.create_node<ccl::SeparateXYZNode>();
        graph.connect(dir, split->input("Vector"));
        auto* up = graph.create_node<ccl::MathNode>();
        up->set_math_type(ccl::NODE_MATH_MAXIMUM);
        graph.connect(split->output("Z"), up->input("Value1"));
        up->set_value2(0.02f);
        auto* reach = graph.create_node<ccl::MathNode>();
        reach->set_math_type(ccl::NODE_MATH_DIVIDE);
        reach->set_value1(2000.0f);
        graph.connect(up->output("Value"), reach->input("Value2"));
        auto* there = graph.create_node<ccl::VectorMathNode>();
        there->set_math_type(ccl::NODE_VECTOR_MATH_SCALE);
        graph.connect(dir, there->input("Vector1"));
        graph.connect(reach->output("Value"), there->input("Scale"));
        // The wind: our x and z are Cycles' x and -y.
        const float a = settings.cloudDirection * kPi / 180.0f, drift = settings.cloudWind * s.time;
        auto* blown = graph.create_node<ccl::VectorMathNode>();
        blown->set_math_type(ccl::NODE_VECTOR_MATH_SUBTRACT);
        graph.connect(there->output("Vector"), blown->input("Vector1"));
        blown->set_vector2(ccl::make_float3(drift * std::cos(a), -drift * std::sin(a), 0.0f));
        ccl::ShaderOutput* n =
            noise(graph, blown->output("Vector"), 1.0f / (1000.0f * std::max(settings.cloudSize, 0.01f)), 6.0f, 0.55f);
        auto range = [&](ccl::ShaderOutput* x, float from, float to) {
            auto* m = graph.create_node<ccl::MapRangeNode>();
            m->set_range_type(ccl::NODE_MAP_RANGE_SMOOTHSTEP);
            m->set_from_min(from);
            m->set_from_max(to);
            m->set_to_min(0.0f);
            m->set_to_max(1.0f);
            m->set_clamp(true);
            graph.connect(x, m->input("Value"));
            return m->output("Result");
        };
        const float edge = 0.66f - 0.36f * cover;
        auto* seen = graph.create_node<ccl::MathNode>();
        seen->set_math_type(ccl::NODE_MATH_MULTIPLY);
        graph.connect(range(n, edge, edge + 0.14f), seen->input("Value1"));
        graph.connect(range(split->output("Z"), 0.0f, 0.15f), seen->input("Value2"));
        // Their light: the sun's on a white surface facing it, more of it
        // looking towards the sun, less where they are thick; the sky's.
        const float k = std::max(skyStrength(s), 1e-6f);
        auto* toward = graph.create_node<ccl::VectorMathNode>();
        toward->set_math_type(ccl::NODE_VECTOR_MATH_DOT_PRODUCT);
        graph.connect(dir, toward->input("Vector1"));
        toward->set_vector2(ccl::normalize(toCycles(s.sunDirection)));
        auto* facing = graph.create_node<ccl::MathNode>();
        facing->set_math_type(ccl::NODE_MATH_MAXIMUM);
        graph.connect(toward->output("Value"), facing->input("Value1"));
        facing->set_value2(0.0f);
        auto* glare = graph.create_node<ccl::MathNode>();
        glare->set_math_type(ccl::NODE_MATH_POWER);
        graph.connect(facing->output("Value"), glare->input("Value1"));
        glare->set_value2(8.0f);
        // Thin edges as bright as white paper in the sun, thick bases a
        // third of it; an overcast -- thick all over -- greyer still.
        ccl::ShaderOutput* lit = scaled(graph, glare->output("Value"), 1.0f, 0.75f);
        lit = scaled(graph, range(n, edge + 0.04f, edge + 0.32f), -0.5f, 0.0f, lit);
        lit = scaled(graph, lit, 1.0f - 0.6f * cover * cover, 0.0f);
        auto* sun = graph.create_node<ccl::ColorNode>();
        sun->set_value(rgb(s.sunLight / k));
        ccl::ShaderOutput* white = times(graph, sun->output("Color"), lit);
        auto* overhead = graph.create_node<ccl::CombineXYZNode>();
        overhead->set_z(1.0f);
        auto* zenith = graph.create_node<ccl::VectorMathNode>();
        zenith->set_math_type(ccl::NODE_VECTOR_MATH_MULTIPLY_ADD);
        graph.connect(daySky(graph, s, false, overhead->output("Vector")), zenith->input("Vector1"));
        zenith->set_vector2(ccl::make_float3(0.5f, 0.5f, 0.5f));
        graph.connect(white, zenith->input("Vector3"));
        auto* mix = graph.create_node<ccl::MixColorNode>();
        graph.connect(seen->output("Value"), mix->input("Factor"));
        graph.connect(sky, mix->input("A"));
        graph.connect(zenith->output("Vector"), mix->input("B"));
        return mix->output("Result");
    }

    ccl::Object* place(ccl::Scene* scene, ccl::Geometry* geometry, const ccl::Transform& tfm, const Vec3& tint) {
        auto* object = scene->create_node<ccl::Object>();
        object->set_geometry(geometry);
        object->set_tfm(tfm);
        object->set_color(rgb(tint));
        objects.push_back(object);
        return object;
    }

    /// The floor: the look's ground, matt, fading out from 35 % of the way
    /// to where it ends -- as the viewport's; seen from above alone. Under a
    /// physical sky, the ground out to the horizon, as uneven as real ground
    /// is (Settings::detail).
    ccl::Shader* floorShader(ccl::Scene* scene, const Scene& s, bool horizon) {
        auto graph = std::make_unique<ccl::ShaderGraph>();
        auto* bsdf = principled(*graph);
        bsdf->set_base_color(rgb(s.look.groundColor));
        bsdf->set_roughness(s.floorMaterial.roughness);
        auto* clear = graph->create_node<ccl::TransparentBsdfNode>();
        clear->set_color(ccl::one_float3());
        auto* geometry = graph->create_node<ccl::GeometryNode>();
        if (settings.detail > 0.0f) {
            // Patches of a few metres too, and bumps of a few centimetres.
            auto* base = graph->create_node<ccl::ColorNode>();
            base->set_value(rgb(s.look.groundColor));
            const float amount = std::clamp(settings.detail, 0.0f, 1.0f);
            ccl::ShaderOutput* patches = noise(*graph, geometry->output("Position"), 0.12f, 3.0f, 0.5f);
            ccl::ShaderOutput* lighter = scaled(*graph, patches, 0.5f * amount, 1.0f - 0.25f * amount);
            const Detail d = detailOf(*graph, geometry->output("Position"), times(*graph, base->output("Color"), lighter),
                                      s.floorMaterial.roughness, amount, 25.0f, 0.3f);
            graph->connect(d.color, bsdf->input("Base Color"));
            graph->connect(d.roughness, bsdf->input("Roughness"));
            graph->connect(d.normal, bsdf->input("Normal"));
        }
        if (horizon) {
            // To the horizon, hazy far off as the air between scatters the
            // sky's light: the sky just above the horizon the way the eye
            // looks, as much of it as 1 - e^(-distance / 3 km).
            auto* eye = graph->create_node<ccl::CameraNode>();
            auto* far = scaled(*graph, eye->output("View Distance"), -1.0f / 3000.0f, 0.0f);
            auto* fade = graph->create_node<ccl::MathNode>();
            fade->set_math_type(ccl::NODE_MATH_EXPONENT);
            graph->connect(far, fade->input("Value1"));
            ccl::ShaderOutput* haze = scaled(*graph, fade->output("Value"), -1.0f, 1.0f);
            auto* split = graph->create_node<ccl::SeparateXYZNode>();
            graph->connect(geometry->output("Incoming"), split->input("Vector"));
            auto* along = graph->create_node<ccl::CombineXYZNode>();
            graph->connect(scaled(*graph, split->output("X"), -1.0f, 0.0f), along->input("X"));
            graph->connect(scaled(*graph, split->output("Y"), -1.0f, 0.0f), along->input("Y"));
            along->set_z(0.03f);
            auto* glow = graph->create_node<ccl::EmissionNode>();
            glow->set_strength(skyStrength(s));
            graph->connect(skyAt(*graph, s, along->output("Vector"), true), glow->input("Color"));
            auto* hazy = graph->create_node<ccl::MixClosureNode>();
            graph->connect(haze, hazy->input("Fac"));
            graph->connect(bsdf->output("BSDF"), hazy->input("Closure1"));
            graph->connect(glow->output("Emission"), hazy->input("Closure2"));
            // Seen from below alone, it is not there.
            auto* mix = graph->create_node<ccl::MixClosureNode>();
            graph->connect(geometry->output("Backfacing"), mix->input("Fac"));
            graph->connect(hazy->output("Closure"), mix->input("Closure1"));
            graph->connect(clear->output("BSDF"), mix->input("Closure2"));
            graph->connect(mix->output("Closure"), graph->output()->input("Surface"));
            auto* shader = scene->create_node<ccl::Shader>();
            shader->set_graph(std::move(graph));
            shader->tag_update(scene);
            return shader;
        }
        auto* split = graph->create_node<ccl::SeparateXYZNode>();
        graph->connect(geometry->output("Position"), split->input("Vector"));
        // How far out: sqrt(x^2 + y^2) of Cycles' world, our x and z.
        auto* xx = graph->create_node<ccl::MathNode>();
        xx->set_math_type(ccl::NODE_MATH_MULTIPLY);
        graph->connect(split->output("X"), xx->input("Value1"));
        graph->connect(split->output("X"), xx->input("Value2"));
        auto* yy = graph->create_node<ccl::MathNode>();
        yy->set_math_type(ccl::NODE_MATH_MULTIPLY);
        graph->connect(split->output("Y"), yy->input("Value1"));
        graph->connect(split->output("Y"), yy->input("Value2"));
        auto* sum = graph->create_node<ccl::MathNode>();
        sum->set_math_type(ccl::NODE_MATH_ADD);
        graph->connect(xx->output("Value"), sum->input("Value1"));
        graph->connect(yy->output("Value"), sum->input("Value2"));
        auto* away = graph->create_node<ccl::MathNode>();
        away->set_math_type(ccl::NODE_MATH_SQRT);
        graph->connect(sum->output("Value"), away->input("Value1"));
        auto* fade = graph->create_node<ccl::MapRangeNode>();
        fade->set_range_type(ccl::NODE_MAP_RANGE_SMOOTHSTEP);
        fade->set_from_min(0.35f * s.floorRadius);
        fade->set_from_max(s.floorRadius);
        fade->set_to_min(0.0f);
        fade->set_to_max(1.0f);
        fade->set_clamp(true);
        graph->connect(away->output("Value"), fade->input("Value"));
        auto* gone = graph->create_node<ccl::MathNode>();
        gone->set_math_type(ccl::NODE_MATH_MAXIMUM);
        graph->connect(fade->output("Result"), gone->input("Value1"));
        graph->connect(geometry->output("Backfacing"), gone->input("Value2"));
        auto* mix = graph->create_node<ccl::MixClosureNode>();
        graph->connect(gone->output("Value"), mix->input("Fac"));
        graph->connect(bsdf->output("BSDF"), mix->input("Closure1"));
        graph->connect(clear->output("BSDF"), mix->input("Closure2"));
        graph->connect(mix->output("Closure"), graph->output()->input("Surface"));
        auto* shader = scene->create_node<ccl::Shader>();
        shader->set_graph(std::move(graph));
        shader->tag_update(scene);
        return shader;
    }

    /// The world: Nishita's sky or a picture all round (skyAt()); else the
    /// look's sky -- with Sky Behind, the viewport's, brighter round the
    /// sun, as a picture all round; without, its colour. Without Sky Behind
    /// the camera sees the studio's backdrop, darker at the bottom of the
    /// picture.
    void world(ccl::Scene* scene, const Scene& s) {
        auto graph = std::make_unique<ccl::ShaderGraph>();
        auto* sky = graph->create_node<ccl::BackgroundNode>();
        sky->set_strength(1.0f);
        // Without Sky Behind the camera sees a studio's backdrop: from the
        // bottom to the top of the picture, Scene::background's grey.
        auto backdropBehind = [&]() {
            const float e = 1.0f / std::max(s.look.exposure, 1e-6f);
            auto* coordinates = graph->create_node<ccl::TextureCoordinateNode>();
            auto* split = graph->create_node<ccl::SeparateXYZNode>();
            graph->connect(coordinates->output("Window"), split->input("Vector"));
            auto* gradient = graph->create_node<ccl::MixColorNode>();
            gradient->set_a(ccl::make_float3(0.022f * e, 0.023f * e, 0.027f * e));
            gradient->set_b(ccl::make_float3(0.075f * e, 0.082f * e, 0.095f * e));
            graph->connect(split->output("Y"), gradient->input("Factor"));
            auto* backdrop = graph->create_node<ccl::BackgroundNode>();
            backdrop->set_strength(1.0f);
            graph->connect(gradient->output("Result"), backdrop->input("Color"));
            auto* path = graph->create_node<ccl::LightPathNode>();
            auto* mix = graph->create_node<ccl::MixClosureNode>();
            graph->connect(path->output("Is Camera Ray"), mix->input("Fac"));
            graph->connect(sky->output("Background"), mix->input("Closure1"));
            graph->connect(backdrop->output("Background"), mix->input("Closure2"));
            graph->connect(mix->output("Closure"), graph->output()->input("Surface"));
        };
        if (skyKind(s) != Settings::Sky::Look) {
            // A real day's -- Nishita's sky, as Blender's Sky Texture, its
            // clouds -- or the picture all round.
            graph->connect(skyAt(*graph, s, nullptr), sky->input("Color"));
            sky->set_strength(skyStrength(s));
            if (s.look.skyBehind) graph->connect(sky->output("Background"), graph->output()->input("Surface"));
            else backdropBehind();
        } else if (s.look.skyBehind) {
            constexpr int kW = 1024, kH = 512;
            std::vector<float> rgba(static_cast<size_t>(kW) * kH * 4, 1.0f);
            for (int j = 0; j < kH; ++j) {
                // Cycles' v: 0 straight down, 1 straight up -- row j of the picture.
                const float v = (static_cast<float>(j) + 0.5f) / kH;
                const float theta = kPi - kPi * v;
                for (int i = 0; i < kW; ++i) {
                    const float u = (static_cast<float>(i) + 0.5f) / kW;
                    const float phi = kPi - 2.0f * kPi * u;
                    const float c[3] = {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
                    const Vec3 light = s.sky(fromCycles(c));
                    float* p = &rgba[(static_cast<size_t>(j) * kW + static_cast<size_t>(i)) * 4];
                    p[0] = light.x;
                    p[1] = light.y;
                    p[2] = light.z;
                }
            }
            auto* env = graph->create_node<ccl::EnvironmentTextureNode>();
            env->handle = scene->image_manager->add_image(std::make_unique<SkyImage>(std::move(rgba), kW, kH, ++imageId),
                                                          env->image_params());
            graph->connect(env->output("Color"), sky->input("Color"));
            graph->connect(sky->output("Background"), graph->output()->input("Surface"));
        } else {
            sky->set_color(rgb(s.skyLight));
            backdropBehind();
        }
        ccl::Shader* shader = scene->default_background;
        shader->set_graph(std::move(graph));
        shader->tag_update(scene);
        scene->background->set_shader(shader);
        scene->background->set_use_shader(true);
        // Sampled as a light, as Blender has it: rays sent towards where
        // the sky is bright -- the sun in a physical sky found.
        auto* dome = scene->create_node<ccl::Light>();
        dome->set_light_type(ccl::LIGHT_BACKGROUND);
        dome->set_use_mis(true);
        ccl::array<ccl::Node*> used;
        used.push_back_slow(shader);
        dome->set_used_shaders(used);
        owned.push_back(dome);
        place(scene, dome, ccl::transform_identity(), Vec3(1.0f, 1.0f, 1.0f));
    }

    /// The look's sun: a distant light as wide as Sun Angle, lighting a
    /// surface facing it as the viewport's does -- an irradiance of pi
    /// times the sun's light -- under Nishita's sky too; under a sky
    /// picture, with Sky Sun alone.
    void sun(ccl::Scene* scene, const Scene& s, const Settings& settings) {
        if (!(std::max({s.sunLight.x, s.sunLight.y, s.sunLight.z}) > 0.0f)) return;
        const Settings::Sky kind = skyKind(s);
        if (kind == Settings::Sky::Image && !settings.skySun) return;  // the picture's own sun lights it
        // Behind a cover of clouds -- more than broken -- most of it hidden.
        float through = 1.0f;
        if (kind == Settings::Sky::Physical) {
            const float t = std::clamp((settings.clouds - 0.6f) / 0.4f, 0.0f, 1.0f);
            through = 1.0f - 0.85f * t * t * (3.0f - 2.0f * t);
        }
        auto* light = scene->create_node<ccl::Light>();
        light->set_light_type(ccl::LIGHT_DISTANT);
        light->set_strength(rgb(s.sunLight * (kPi * through)));
        light->set_angle(std::clamp(settings.sunAngle, 0.01f, 30.0f) * kPi / 180.0f);
        light->set_use_mis(true);
        light->set_cast_shadow(true);
        // Its light white and whole: the strength is the light's (Cycles'
        // default light shader gives none off).
        if (!sunShader) {
            auto graph = std::make_unique<ccl::ShaderGraph>();
            auto* emission = graph->create_node<ccl::EmissionNode>();
            emission->set_color(ccl::one_float3());
            emission->set_strength(1.0f);
            graph->connect(emission->output("Emission"), graph->output()->input("Surface"));
            sunShader = scene->create_node<ccl::Shader>();
            sunShader->set_graph(std::move(graph));
            sunShader->tag_update(scene);
        }
        ccl::array<ccl::Node*> used;
        used.push_back_slow(sunShader);
        light->set_used_shaders(used);
        owned.push_back(light);
        // Its -Z the way the light goes: Z towards the sun.
        const ccl::float3 z = ccl::normalize(toCycles(s.sunDirection));
        const ccl::float3 x = ccl::normalize(std::fabs(z.z) < 0.9f ? ccl::cross(ccl::make_float3(0.0f, 0.0f, 1.0f), z)
                                                                     : ccl::cross(ccl::make_float3(1.0f, 0.0f, 0.0f), z));
        const ccl::float3 y = ccl::cross(z, x);
        const ccl::Transform tfm = ccl::make_transform(x.x, y.x, z.x, 0.0f, x.y, y.y, z.y, 0.0f, x.z, y.z, z.z, 0.0f);
        ccl::Object* object = place(scene, light, tfm, Vec3(1.0f, 1.0f, 1.0f));
        object->set_visibility(ccl::PATH_RAY_ALL_VISIBILITY & ~ccl::PATH_RAY_CAMERA);
    }

    /// The camera: the shot's, looking along its -z, its y up the picture
    /// -- Cycles' looks along its +z; its lens, the f-number and the focus.
    void camera(ccl::Scene* scene, const Scene& s, const Settings& settings) {
        const sim::Camera& c = s.camera;
        const Vec3 right = c.right(), up = c.up(), forward = c.forward();
        const ccl::float3 r = toCycles(right), u = toCycles(up), f = toCycles(forward), p = toCycles(c.position);
        ccl::Camera* cam = scene->camera;
        cam->set_matrix(ccl::make_transform(r.x, u.x, f.x, p.x, r.y, u.y, f.y, p.y, r.z, u.z, f.z, p.z));
        cam->set_camera_type(ccl::CAMERA_PERSPECTIVE);
        const float fovY = c.fovY() * kPi / 180.0f;
        const float aspect = static_cast<float>(settings.width) / static_cast<float>(std::max(settings.height, 1));
        // Cycles' angle is across the shorter side of the picture.
        cam->set_fov(aspect >= 1.0f ? fovY : 2.0f * std::atan(std::tan(0.5f * fovY) * aspect));
        {
            std::lock_guard<std::mutex> lock(pictures.mutex);
            pictures.tanY = std::tan(0.5f * fovY);
            pictures.tanX = pictures.tanY * aspect;
        }
        cam->set_full_width(settings.width);
        cam->set_full_height(settings.height);
        cam->set_nearclip(1e-3f);
        cam->set_farclip(1e5f);
        if (settings.fstop > 0.0f) {
            // As Blender has it: the lens's radius, half its focal length
            // over the f-number.
            cam->set_aperturesize(0.5f * c.focal * 1e-3f / settings.fstop);
            float focus = settings.focus;
            if (!(focus > 0.0f)) {
                Hit hit;
                focus = s.intersect(c.position, forward, 1e30f, 0.0f, hit) ? hit.t : 10.0f;
            }
            cam->set_focaldistance(focus);
        } else {
            cam->set_aperturesize(0.0f);
        }
        cam->compute_auto_viewplane();
        cam->need_flags_update = true;
        cam->need_device_update = true;
    }

    /// The smoke and the fire: a box round the cells that hold any, inside
    /// it what they stop and give off read from grids as ours reads them
    /// (Gas::dense), the smoke scattering in its colour -- forwards, as our
    /// two lobes do on the whole -- Cycles stepping through it a cell at a
    /// time.
    void gas(ccl::Scene* scene, const Scene& s) {
        if (!s.gas) return;
        const Gas::Dense d = s.gas->dense(s.gasLook, kMostGasCells);
        const size_t n = static_cast<size_t>(d.size[0]) * static_cast<size_t>(d.size[1]) * static_cast<size_t>(d.size[2]);
        if (n == 0 || d.extinction.size() != n) return;
        const Vec3 lo = d.box.lo, hi = d.box.hi, size = hi - lo;
        if (!(size.x > 0.0f && size.y > 0.0f && size.z > 0.0f)) return;
        const bool glows = d.emission.size() == n;

        if (!gasShader) gasShader = scene->create_node<ccl::Shader>();
        auto graph = std::make_unique<ccl::ShaderGraph>();
        auto* volume = graph->create_node<ccl::PrincipledVolumeNode>();
        volume->set_color(rgb(s.gasLook.albedo));
        volume->set_absorption_color(ccl::zero_float3());
        volume->set_anisotropy(0.7f * 0.55f - 0.3f * 0.25f);
        auto* stops = graph->create_node<ccl::AttributeNode>();
        stops->set_attribute(ccl::ustring("pg_extinction"));
        graph->connect(stops->output("Fac"), volume->input("Density"));
        if (glows) {
            auto* gives = graph->create_node<ccl::AttributeNode>();
            gives->set_attribute(ccl::ustring("pg_emission"));
            graph->connect(gives->output("Color"), volume->input("Emission Color"));
            volume->set_emission_strength(1.0f);
        }
        graph->connect(volume->output("Volume"), graph->output()->input("Volume"));
        gasShader->set_graph(std::move(graph));
        gasShader->set_heterogeneous_volume(true);
        // A step a cell: Cycles takes a tenth of the box, times this.
        const float step = size.x / static_cast<float>(d.size[0]);
        gasShader->set_volume_step_rate(step / (0.1f * (size.x + size.y + size.z + 6.0f * step) / 3.0f));
        gasShader->tag_update(scene);

        // The box, its faces out, a cell bigger all round than the grids --
        // the floor in it, not in its bottom face -- where it is in Cycles'
        // world itself, not turned onto its side as the rest: Cycles moves
        // the corners of a mesh one object places to where it places them.
        const float cell = size.x / static_cast<float>(d.size[0]);
        const Vec3 blo = lo - Vec3(cell, cell, cell), bhi = hi + Vec3(cell, cell, cell);
        const Vec3 c[8] = {{blo.x, blo.y, blo.z}, {bhi.x, blo.y, blo.z}, {bhi.x, bhi.y, blo.z}, {blo.x, bhi.y, blo.z},
                           {blo.x, blo.y, bhi.z}, {bhi.x, blo.y, bhi.z}, {bhi.x, bhi.y, bhi.z}, {blo.x, bhi.y, bhi.z}};
        const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
        const Vec3 out[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
        auto zUp = [](const Vec3& v) { return Vec3(v.x, -v.z, v.y); };
        std::vector<Vec3> points, normals;
        for (int f = 0; f < 6; ++f) {
            for (const int k : {0, 1, 2, 0, 2, 3}) {
                points.push_back(zUp(c[faces[f][k]]));
                normals.push_back(zUp(out[f]));
            }
        }
        ccl::Mesh* mesh = ownMesh(scene, points, normals, Vec3(1.0f, 1.0f, 1.0f), gasShader);
        // Where in the grids a point of Cycles' world is: 0 to 1 across them
        // along our x, y and z -- Cycles' x, z and -y; nothing outside.
        const ccl::Transform where = ccl::make_transform(1.0f / size.x, 0.0f, 0.0f, -lo.x / size.x,  //
                                                         0.0f, 0.0f, 1.0f / size.y, -lo.y / size.y,  //
                                                         0.0f, -1.0f / size.z, 0.0f, -lo.z / size.z);
        ccl::ImageParams params;
        params.interpolation = ccl::INTERPOLATION_LINEAR;
        params.extension = ccl::EXTENSION_CLIP;
        ccl::Attribute* stopped = mesh->attributes.add(ccl::ustring("pg_extinction"), ccl::TypeFloat, ccl::ATTR_ELEMENT_VOXEL);
        stopped->data_voxel() =
            scene->image_manager->add_image(std::make_unique<VoxelImage>(d.extinction, 1, d.size, where, ++imageId), params);
        if (glows) {
            std::vector<float> rgba(4 * n);
            for (size_t i = 0; i < n; ++i) {
                rgba[4 * i] = d.emission[i].x;
                rgba[4 * i + 1] = d.emission[i].y;
                rgba[4 * i + 2] = d.emission[i].z;
                rgba[4 * i + 3] = 1.0f;  // not a share of it to divide by
            }
            ccl::Attribute* given = mesh->attributes.add(ccl::ustring("pg_emission"), ccl::TypeColor, ccl::ATTR_ELEMENT_VOXEL);
            given->data_voxel() =
                scene->image_manager->add_image(std::make_unique<VoxelImage>(std::move(rgba), 4, d.size, where, ++imageId), params);
        }
        place(scene, mesh, ccl::transform_identity(), Vec3(1.0f, 1.0f, 1.0f));
    }

    void sync(const Scene& s, const Settings& settings) {
        ccl::Scene* scene = session->scene.get();
        waterGlow = s.look.waterColor * (s.skyLight * 1.5f + s.sunLight * (0.35f * std::max(s.sunDirection.y, 0.0f)));
        // What the last scene had of its own goes; the meshes stay.
        if (!objects.empty()) {
            std::set<ccl::Object*> gone(objects.begin(), objects.end());
            scene->delete_nodes(gone);
            objects.clear();
        }
        for (ccl::Geometry* g : owned) {
            if (g->is_light()) scene->delete_node(static_cast<ccl::Light*>(g));
            else scene->delete_node(static_cast<ccl::Mesh*>(g));
        }
        owned.clear();
        for (auto it = meshes.begin(); it != meshes.end();) {
            if (it->second.mesh.expired()) {
                scene->delete_node(it->second.cycles);
                it = meshes.erase(it);
            } else {
                ++it;
            }
        }

        // The meshes, where they stand.
        for (const Placed& p : s.placed) {
            if (p.mesh >= s.meshes.size() || !s.meshes[p.mesh] || s.meshes[p.mesh]->count() == 0) continue;
            ccl::Mesh* mesh = meshOf(scene, s.meshes[p.mesh], s.look);
            place(scene, mesh, placement(p.axes, p.scale, p.at), p.tint);
        }
        // The scene's objects, each its own mesh, its colour its object's.
        const ccl::Transform turned = placement(Mat3(1.0f), 1.0f, Vec3());
        for (const sim::Solid& solid : s.solids) {
            std::vector<Vec3> points, normals;
            tessellate(solid.body.instance(), points, normals);
            if (points.empty()) continue;
            ccl::Mesh* mesh = ownMesh(scene, points, normals, Vec3(1.0f, 1.0f, 1.0f), shaderOf(scene, s.solidMaterial, s.look));
            ccl::Object* object = place(scene, mesh, turned, solid.color);
            if (solid.matte == sim::Matte::Holdout) object->set_use_holdout(true);
            if (solid.matte == sim::Matte::Catcher) object->set_is_shadow_catcher(true);
        }
        // The floor: as far as it goes, a square round it, faded to a disc.
        if (s.look.floor) {
            const bool horizon = skyKind(s) != Settings::Sky::Look && s.look.skyBehind;
            const float r = horizon ? 5000.0f : std::min(s.floorRadius, 1e5f);
            const Vec3 a(-r, 0.0f, -r), b(r, 0.0f, -r), c(r, 0.0f, r), d(-r, 0.0f, r), n(0.0f, 1.0f, 0.0f);
            ccl::Mesh* mesh = ownMesh(scene, {a, d, c, a, c, b}, {n, n, n, n, n, n}, s.look.groundColor,
                                      floorShader(scene, s, horizon));
            place(scene, mesh, turned, Vec3(1.0f, 1.0f, 1.0f));
        }
        gas(scene, s);
        world(scene, s);
        sun(scene, s, settings);
        camera(scene, s, settings);

        ccl::Integrator* integrator = scene->integrator;
        integrator->set_max_bounce(settings.bounces);
        integrator->set_max_diffuse_bounce(settings.bounces);
        integrator->set_max_glossy_bounce(settings.bounces);
        integrator->set_max_transmission_bounce(std::max(settings.bounces, 8));
        integrator->set_max_volume_bounce(settings.bounces);
        integrator->set_transparent_max_bounce(16);
        integrator->set_sample_clamp_indirect(settings.clamp);
        integrator->set_seed(static_cast<int>(settings.seed));
        integrator->set_use_adaptive_sampling(!interactive);
        const bool denoise = settings.denoise && pictures.denoise;
        integrator->set_use_denoise(denoise);
        integrator->set_denoiser_type(ccl::DENOISER_OPENIMAGEDENOISE);
        integrator->set_use_denoise_pass_albedo(true);
        integrator->set_use_denoise_pass_normal(true);
        integrator->set_denoiser_prefilter(interactive ? ccl::DENOISER_PREFILTER_FAST : ccl::DENOISER_PREFILTER_ACCURATE);
        integrator->set_denoise_start_sample(1);
        scene->film->set_exposure(1.0f);
        // What a pixel first sees, for the passes: a surface at least half
        // there -- the floor fading out, as ours.
        scene->film->set_pass_alpha_threshold(0.5f);

        // The passes read, by their names: the light, and what the camera's
        // rays meet. (A scene starts with a combined pass of no name.)
        auto pass = [&](ccl::PassType type, const char* name, ccl::PassMode mode) {
            for (ccl::Pass* p : scene->passes) {
                if (p->get_name() == ccl::ustring(name)) {
                    p->set_mode(mode);
                    return;
                }
            }
            ccl::Pass* p = scene->create_node<ccl::Pass>();
            p->set_type(type);
            p->set_name(ccl::ustring(name));
            p->set_mode(mode);
        };
        pass(ccl::PASS_COMBINED, "combined", denoise ? ccl::PassMode::DENOISED : ccl::PassMode::NOISY);
        pass(ccl::PASS_DENOISING_ALBEDO, "albedo", ccl::PassMode::NOISY);
        pass(ccl::PASS_NORMAL, "normal", ccl::PassMode::NOISY);
        pass(ccl::PASS_DEPTH, "depth", ccl::PassMode::NOISY);
    }
};

bool cyclesAvailable() { return true; }
std::string cyclesVersion() { return std::string("Cycles ") + CYCLES_VERSION_STRING; }
std::string cyclesDenoiser() {
#ifdef WITH_OPENIMAGEDENOISE
    return "Open Image Denoise";
#else
    return {};
#endif
}

CyclesRender::CyclesRender(bool interactive) : impl_(std::make_unique<Impl>()) {
    impl_->interactive = interactive;
#ifdef WITH_OPENIMAGEDENOISE
    impl_->pictures.denoise = true;
#endif
    // The Render tab's session lives as long as this: asked how far it got
    // from the window's thread while start() goes on on another, it is
    // there, not being made.
    if (interactive) impl_->make();
}

CyclesRender::~CyclesRender() {
    if (impl_->session) {
        impl_->session->cancel(true);
        impl_->session.reset();
    }
}

void CyclesRender::start(std::shared_ptr<const Scene> scene, const Settings& settings) {
    Impl& m = *impl_;
    if (!scene) return;
    if (!m.session || !m.interactive) {
        // A render to the end is a session of its own.
        if (m.session) {
            m.session->cancel(true);
            m.session.reset();
            m.meshes.clear();
            m.shaders.clear();
            m.sunShader = nullptr;
            m.gasShader = nullptr;
            m.objects.clear();
            m.owned.clear();
        }
        m.make();
        m.started = false;
    } else if (m.session->progress.get_cancel()) {
        // Stopped (cancel()): its thread went back to waiting, the stop
        // still marked -- cleared, the next render starts it again.
        m.session->progress.reset();
        m.started = false;
    }
    m.settings = settings;
    {
        std::lock_guard<std::mutex> lock(m.pictures.mutex);
        m.pictures.fresh = false;
        m.pictures.beauty = Image();
        m.pictures.albedo = Image();
        m.pictures.normal = Image();
        m.pictures.depth = Image();
    }
    {
        ccl::thread_scoped_lock lock(m.session->scene->mutex);
        m.sync(*scene, settings);
    }
    m.params.samples = std::max(settings.samples, 1);
    ccl::BufferParams buffer;
    buffer.width = settings.width;
    buffer.height = settings.height;
    buffer.full_width = settings.width;
    buffer.full_height = settings.height;
    m.session->reset(m.params, buffer);
    if (!m.started) {
        m.session->start();
        m.started = true;
    }
}

void CyclesRender::setPaused(bool paused) {
    if (impl_->session) impl_->session->set_pause(paused);
}

void CyclesRender::cancel() {
    if (impl_->session) impl_->session->cancel(true);
}

void CyclesRender::wait() {
    if (impl_->session && impl_->started) impl_->session->wait();
}

bool CyclesRender::done() const {
    if (!impl_->session) return false;
    const ccl::Progress& p = impl_->session->progress;
    return p.get_progress() >= 1.0 - 1e-9 || p.get_cancel() || p.get_error();
}

int CyclesRender::samples() const { return impl_->session ? impl_->session->progress.get_current_sample() : 0; }

double CyclesRender::seconds() const {
    if (!impl_->session) return 0.0;
    double total = 0.0, render = 0.0;
    impl_->session->progress.get_time(total, render);
    return render;
}

std::string CyclesRender::error() const {
    if (!impl_->session || !impl_->session->progress.get_error()) return {};
    return impl_->session->progress.get_error_message();
}

bool CyclesRender::takePicture(Image& beauty) {
    std::lock_guard<std::mutex> lock(impl_->pictures.mutex);
    if (!impl_->pictures.fresh) return false;
    beauty = impl_->pictures.beauty;
    impl_->pictures.fresh = false;
    return true;
}

Image CyclesRender::beauty() const {
    std::lock_guard<std::mutex> lock(impl_->pictures.mutex);
    return impl_->pictures.beauty;
}
Image CyclesRender::albedo() const {
    std::lock_guard<std::mutex> lock(impl_->pictures.mutex);
    return impl_->pictures.albedo;
}
Image CyclesRender::normal() const {
    std::lock_guard<std::mutex> lock(impl_->pictures.mutex);
    return impl_->pictures.normal;
}
Image CyclesRender::depth() const {
    std::lock_guard<std::mutex> lock(impl_->pictures.mutex);
    return impl_->pictures.depth;
}

#else  // PG_HAVE_CYCLES

struct CyclesRender::Impl {};

bool cyclesAvailable() { return false; }
std::string cyclesVersion() { return {}; }
std::string cyclesDenoiser() { return {}; }
CyclesRender::CyclesRender(bool) : impl_(std::make_unique<Impl>()) {}
CyclesRender::~CyclesRender() = default;
void CyclesRender::start(std::shared_ptr<const Scene>, const Settings&) {}
void CyclesRender::setPaused(bool) {}
void CyclesRender::cancel() {}
void CyclesRender::wait() {}
bool CyclesRender::done() const { return true; }
int CyclesRender::samples() const { return 0; }
double CyclesRender::seconds() const { return 0.0; }
std::string CyclesRender::error() const { return "built without Cycles"; }
bool CyclesRender::takePicture(Image&) { return false; }
Image CyclesRender::beauty() const { return {}; }
Image CyclesRender::albedo() const { return {}; }
Image CyclesRender::normal() const { return {}; }
Image CyclesRender::depth() const { return {}; }

#endif  // PG_HAVE_CYCLES

}  // namespace pg::render
