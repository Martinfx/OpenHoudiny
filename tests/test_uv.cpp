//
// Texture coordinates and normal maps: UV Project lays uv on the corners --
// from a plane, a box's six sides, round a cylinder and a sphere, each
// picture upright and none mirrored as seen from outside, the faces across
// the seam and over a pole kept whole; OBJ brings texture coordinates in and
// takes uv out; the renderers' meshes carry the uv and its tangents, the
// Material node's Projection and Normal Strength; texture sets find their
// normal maps, OpenGL's or DirectX's; a normal bends as its map says; and
// both renderers lay a picture on by uv and bend the light by its normal map.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/io/Obj.h"
#include "pg/io/Picture.h"
#include "pg/nodes/Nodes.h"
#include "pg/render/Cycles.h"
#include "pg/render/PathTracer.h"
#include "pg/render/Scene.h"
#include "pg/render/Textures.h"

#include "test_framework.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>

using namespace pg;
namespace fs = std::filesystem;

namespace {

/// A node of `type` set up by `setup`, cooked over `input` (none for a
/// node that makes geometry).
GeometryPtr cooked(const std::string& type, GeometryPtr input, const std::function<void(Node&)>& setup) {
    registerBuiltinNodes();
    Graph g;
    Node* node = g.create(type, "node");
    setup(*node);
    if (!input) {
        CookEngine engine;
        return engine.cook(*node, CookContext{});
    }
    const GeometryPtr in[1] = {std::move(input)};
    return node->cookNode(CookContext{}, in);
}

/// The uv of each corner of `geo`, and its point, primitive by primitive.
struct Corner {
    size_t prim;
    Vec3 p, uv;
};
std::vector<std::vector<Corner>> cornersOf(const Geometry& geo) {
    std::vector<std::vector<Corner>> out;
    const AttributeArray* uv = geo.vertices().find("uv");
    if (!uv || uv->type() != AttrType::Vec3) return out;
    const auto P = geo.positions();
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto pts = geo.primitivePoints(prim);
        const size_t first = geo.primitiveVertexStart(prim);
        out.emplace_back();
        for (size_t k = 0; k < pts.size(); ++k) out.back().push_back({prim, P[pts[k]], uv->read<Vec3>()[first + k]});
    }
    return out;
}

/// Twice the area a face's corners make in uv, with its sign: above 0 where
/// the picture goes round the face as the face goes round its outward
/// normal -- not mirrored as seen from outside.
float uvArea(const std::vector<Corner>& face) {
    float a = 0.0f;
    for (size_t k = 0; k < face.size(); ++k) {
        const Vec3 &p = face[k].uv, &q = face[(k + 1) % face.size()].uv;
        a += p.x * q.y - q.x * p.y;
    }
    return a;
}

/// A picture of `width` x `height`, `pixel(x, y)` its colour (as shown, the
/// top row first), to `path`.
void writePicture(const std::string& path, int width, int height, const std::function<Vec3(int, int)>& pixel) {
    io::Picture p;
    p.width = width;
    p.height = height;
    p.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Vec3 c = pixel(x, y);
            float* q = &p.rgba[(static_cast<size_t>(y) * width + x) * 4];
            q[0] = c.x;
            q[1] = c.y;
            q[2] = c.z;
            q[3] = 1.0f;
        }
    }
    std::string error;
    CHECK(io::writePicture(path, p, 95, error));
}

/// A square of ground, 4 m, facing up -- grey, of the texture `texture` laid
/// on by its uv: across it from -x to +x in u, from +z to -z in v, a picture.
std::shared_ptr<Geometry> uvGround(const std::string& texture) {
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(4);
    auto P = geo->positionsForWrite();
    P[0] = Vec3(-2.0f, 0.0f, -2.0f);
    P[1] = Vec3(-2.0f, 0.0f, 2.0f);
    P[2] = Vec3(2.0f, 0.0f, 2.0f);
    P[3] = Vec3(2.0f, 0.0f, -2.0f);
    const uint32_t quad[4] = {0, 1, 2, 3};
    geo->addPrimitive(quad, true);
    geo->primitives().create("Cd", AttrType::Vec3).write<Vec3>()[0] = Vec3(0.5f);
    GeometryPtr laid = cooked("uvproject", geo, [](Node& n) {
        n.setVec3("center", Vec3(-2.0f, 0.0f, 2.0f));
        n.setFloat("scale", 4.0f);
    });
    laid = cooked("material", laid, [&](Node& n) {
        n.setInt("material", 0);
        n.setString("texture", texture);
        n.setBool("texture_tint", false);
    });
    return std::const_pointer_cast<Geometry>(laid);
}

/// The ground from above, under the look `look`, `size` pixels square.
render::SceneInput fromAbove(const GeometryPtr& geo, const sim::Look& look, int size) {
    sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 5.0f, 0.01f), Vec3(0.0f, 0.0f, 0.0f));
    cam.width = size;
    cam.height = size;
    render::SceneInput in;
    in.geometry = geo;
    in.look = look;
    in.camera = cam;
    return in;
}

render::Image traced(const std::shared_ptr<const render::Scene>& scene, const render::Settings& s) {
    render::PathTracer tracer;
    tracer.setSettings(s);
    tracer.setScene(scene);
    while (tracer.samples() < s.samples) tracer.pass();
    return tracer.beauty();
}

render::Image cycled(const std::shared_ptr<const render::Scene>& scene, const render::Settings& s) {
    render::CyclesRender r;
    r.start(scene, s);
    r.wait();
    CHECK(r.error().empty());
    return r.beauty();
}

/// The mean colour of a picture's pixels from (x0, y0) to before (x1, y1).
Vec3 meanOf(const render::Image& img, int x0, int y0, int x1, int y1) {
    Vec3 sum(0.0f);
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float* p = &img.pixels[3 * (static_cast<size_t>(y) * img.width + x)];
            sum = sum + Vec3(p[0], p[1], p[2]);
            ++n;
        }
    }
    return n > 0 ? sum / static_cast<float>(n) : sum;
}

}  // namespace

TEST(uv_project_lays_uv_on_the_corners_seen_from_outside) {
    // A plane from above: u along x, v along -z, two metres a picture.
    const GeometryPtr grid = cooked("grid", nullptr, [](Node& n) {
        n.setInt("rows", 3);
        n.setInt("cols", 3);
        n.setFloat("sizex", 2.0f);
        n.setFloat("sizez", 2.0f);
    });
    const auto planar = cornersOf(*cooked("uvproject", grid, [](Node& n) { n.setFloat("scale", 2.0f); }));
    CHECK(!planar.empty());
    float off = 0.0f;
    for (const auto& face : planar) {
        for (const Corner& c : face) off = std::max(off, length(c.uv - Vec3(c.p.x / 2.0f, -c.p.z / 2.0f, 0.0f)));
    }
    CHECK(off < 1e-6f);

    // A box: each side a picture of its own, upright, none mirrored.
    const GeometryPtr box = cooked("box", nullptr, [](Node& n) { n.setInt("divisions", 2); });
    const auto boxed = cornersOf(*cooked("uvproject", box, [](Node& n) { n.setInt("projection", 1); }));
    CHECK_EQ(boxed.size(), box->primitiveCount());
    int upright = 0;
    for (const auto& face : boxed) {
        CHECK(uvArea(face) > 0.0f);
        // The front (+z) as it is seen: u along x, v up.
        const Vec3 m = (face[0].p + face[1].p + face[2].p) / 3.0f;
        if (m.z > 0.49f) {
            for (const Corner& c : face) off = std::max(off, length(c.uv - Vec3(c.p.x, c.p.y, 0.0f)));
            ++upright;
        }
    }
    CHECK_EQ(upright, 4);
    CHECK(off < 1e-6f);

    // A tube: once round in u -- a half at the front, three quarters to the
    // right -- along it in v; the faces across the seam whole.
    const GeometryPtr tube = cooked("tube", nullptr, [](Node& n) {
        n.setInt("columns", 12);
        n.setBool("caps", false);
        n.setFloat("height", 2.0f);
    });
    const auto round = cornersOf(*cooked("uvproject", tube, [](Node& n) { n.setInt("projection", 2); }));
    CHECK(!round.empty());
    float worst = 0.0f, widest = 0.0f;
    for (const auto& face : round) {
        CHECK(uvArea(face) > 0.0f);
        float lo = 1e9f, hi = -1e9f;
        for (const Corner& c : face) {
            const float want = std::atan2(c.p.x, c.p.z) / (2.0f * 3.14159265f) + 0.5f;
            const float u = c.uv.x - std::floor(c.uv.x), w = want - std::floor(want);
            worst = std::max({worst, std::min(std::fabs(u - w), 1.0f - std::fabs(u - w)), std::fabs(c.uv.y - c.p.y)});
            lo = std::min(lo, c.uv.x);
            hi = std::max(hi, c.uv.x);
        }
        widest = std::max(widest, hi - lo);
    }
    std::printf("  cylinder: off %.2g, the widest face %.3f of a turn\n", worst, widest);
    CHECK(worst < 1e-5f);
    CHECK(widest < 0.1f);

    // A sphere: v from the lower pole to the upper, a corner on a pole the
    // u of the rest of its face.
    const GeometryPtr sphere = cooked("sphere", nullptr, [](Node& n) {
        n.setInt("rows", 6);
        n.setInt("columns", 8);
    });
    const auto ball = cornersOf(*cooked("uvproject", sphere, [](Node& n) { n.setInt("projection", 3); }));
    int poles = 0;
    widest = 0.0f;
    for (const auto& face : ball) {
        CHECK(uvArea(face) > 0.0f);
        float lo = 1e9f, hi = -1e9f;
        for (size_t k = 0; k < face.size(); ++k) {
            lo = std::min(lo, face[k].uv.x);
            hi = std::max(hi, face[k].uv.x);
            if (std::fabs(std::fabs(face[k].p.y) - 0.5f) < 1e-5f) {
                ++poles;
                CHECK(std::fabs(face[k].uv.y - (face[k].p.y > 0.0f ? 1.0f : 0.0f)) < 1e-5f);
                float rest = 0.0f;
                for (size_t j = 0; j < face.size(); ++j) rest += j == k ? 0.0f : face[j].uv.x;
                CHECK(std::fabs(face[k].uv.x - rest / static_cast<float>(face.size() - 1)) < 1e-5f);
            }
        }
        widest = std::max(widest, hi - lo);
    }
    CHECK_EQ(poles, 16);
    CHECK(widest < 0.2f);

    // A group alone: the rest keep what they had.
    const GeometryPtr some = cooked("uvproject", cooked("uvproject", grid, [](Node&) {}), [](Node& n) {
        n.setString("group", "0");
        n.setFloat("scale", 10.0f);
    });
    const auto both = cornersOf(*some);
    CHECK(std::fabs(both[0][0].uv.x - both[0][0].p.x / 10.0f) < 1e-6f);
    CHECK(std::fabs(both[1][0].uv.x - both[1][0].p.x) < 1e-6f);
}

TEST(uv_obj_brings_texture_coordinates_in_and_takes_uv_out) {
    const std::string text =
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1 0\nvt 0.25 0.75\n"
        "f 1/1 2/2/1 3/-2 4/4\n"
        "f 1//1 3 4\n";
    Geometry geo;
    std::string error;
    CHECK(io::parseObj(text, geo, error));
    const AttributeArray* uv = geo.vertices().find("uv");
    CHECK(uv && uv->type() == AttrType::Vec3);
    if (uv) {
        const auto t = uv->read<Vec3>();
        CHECK(t[0] == Vec3(0.0f, 0.0f, 0.0f) && t[1] == Vec3(1.0f, 0.0f, 0.0f) && t[2] == Vec3(1.0f, 1.0f, 0.0f));
        CHECK(t[3] == Vec3(0.25f, 0.75f, 0.0f));
        CHECK(t[4] == Vec3(0.0f) && t[5] == Vec3(0.0f));  // corners without
    }
    // Out and in again: the same.
    const std::string out = io::formatObj(geo);
    CHECK(out.find("vt 0.25 0.75\n") != std::string::npos);
    CHECK(out.find("f 1/1 2/2 3/3 4/4\n") != std::string::npos);
    Geometry back;
    CHECK(io::parseObj(out, back, error));
    const AttributeArray* again = back.vertices().find("uv");
    CHECK(again && again->size() == uv->size());
    if (again && uv) {
        for (size_t i = 0; i < uv->size(); ++i) CHECK(again->read<Vec3>()[i] == uv->read<Vec3>()[i]);
    }
    // A number that is no texture coordinate: none for that corner; none
    // at all, no uv.
    CHECK(io::parseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0.5 0.5\nf 1/1 2/5 3/1\n", geo, error));
    const AttributeArray* some = geo.vertices().find("uv");
    CHECK(some && some->read<Vec3>()[0] == Vec3(0.5f, 0.5f, 0.0f) && some->read<Vec3>()[1] == Vec3(0.0f));
    CHECK(io::parseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", geo, error));
    CHECK(!geo.vertices().find("uv"));
}

TEST(uv_meshes_carry_uv_its_tangents_and_how_the_pictures_are_laid) {
    // From above -- u along x, v along -z -- on a surface facing up: the
    // tangent +x, v along normal x tangent (-z).
    const GeometryPtr grid = cooked("grid", nullptr, [](Node& n) {
        n.setInt("rows", 4);
        n.setInt("cols", 4);
    });
    const GeometryPtr laid = cooked("uvproject", grid, [](Node&) {});
    const GeometryPtr own = cooked("material", laid, [](Node& n) { n.setString("texture", "/x/y_diff.png"); });
    const auto mesh = render::meshOf(*own);
    CHECK_EQ(mesh->materials.size(), 1u);
    CHECK(mesh->materials[0].byUv);
    CHECK_EQ(mesh->uv.size(), 3 * mesh->count());
    CHECK_EQ(mesh->tangents.size(), 3 * mesh->count());
    float off = 0.0f;
    for (const Vec4& t : mesh->tangents) off = std::max(off, length(t - Vec4(1.0f, 0.0f, 0.0f, 1.0f)));
    CHECK(off < 1e-5f);
    // Mirrored -- u along -x, on the points -- the tangent the other way,
    // and v along it: -1.
    GeometryPtr mirrored = cooked("attribwrangle", grid, [](Node& n) {
        n.setString("snippet", "v@uv = set(-@P.x, -@P.z, 0);");
        n.setInt("runover", 0);
    });
    mirrored = cooked("material", mirrored, [](Node& n) {
        n.setString("texture", "/x/y_diff.png");
        n.setInt("texture_projection", 1);
    });
    const auto back = render::meshOf(*mirrored);
    off = 0.0f;
    for (const Vec4& t : back->tangents) off = std::max(off, length(t - Vec4(-1.0f, 0.0f, 0.0f, -1.0f)));
    CHECK(off < 1e-5f);

    // How they are laid: Auto by uv for a texture of one's own, from three
    // sides for the material's photographs; UV asked for, by uv; Three
    // Sides, from three sides; without uv, from three sides whatever is
    // asked. Normal Strength as the node says.
    auto byUv = [&](const GeometryPtr& g, int projection, bool texture) {
        const GeometryPtr m = cooked("material", g, [&](Node& n) {
            n.setInt("material", 1);  // concrete
            if (texture) n.setString("texture", "/x/y_diff.png");
            n.setInt("texture_projection", projection);
            n.setFloat("texture_normal", 0.5f);
        });
        const auto made = render::meshOf(*m);
        CHECK_NEAR(made->materials[0].normalStrength, 0.5f, 1e-6f);
        return made->materials[0].byUv;
    };
    CHECK(byUv(laid, 0, true) && !byUv(laid, 0, false));
    CHECK(byUv(laid, 1, false) && byUv(laid, 1, true));
    CHECK(!byUv(laid, 2, true));
    CHECK(!byUv(grid, 1, true) && !byUv(grid, 0, true));
    CHECK(render::meshOf(*cooked("material", grid, [](Node&) {}))->uv.empty());
    // The strength of the faces outside the group stays 1.
    const GeometryPtr half = cooked("material", laid, [](Node& n) {
        n.setString("group", "0");
        n.setFloat("texture_normal", 0.0f);
    });
    const AttributeArray* strength = half->primitives().find("texture_normal");
    CHECK(strength && strength->read<float>()[0] == 0.0f && strength->read<float>()[1] == 1.0f);
}

TEST(uv_texture_sets_find_their_normal_maps) {
    const fs::path dir = fs::temp_directory_path() / "pg_test_normal_maps";
    fs::remove_all(dir);
    auto touch = [](const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "";
    };
    // Poly Haven's: OpenGL's rather than DirectX's where there are both.
    for (const char* f : {"rock_diff_1k.png", "rock_nor_dx_1k.png", "rock_nor_gl_1k.png", "rock_disp_1k.png",
                          "moss_diff_1k.png", "moss_nor_dx_1k.png"}) {
        touch(dir / "ph" / f);
    }
    const render::TextureSet rock = render::textureSet((dir / "ph" / "rock_diff_1k.png").string());
    CHECK(rock.normal == (dir / "ph" / "rock_nor_gl_1k.png").string() && !rock.normalDirectX);
    const render::TextureSet moss = render::textureSet((dir / "ph" / "moss_diff_1k.png").string());
    CHECK(moss.normal == (dir / "ph" / "moss_nor_dx_1k.png").string() && moss.normalDirectX);
    // ambientCG's.
    for (const char* f : {"Ground054_1K-PNG_Color.png", "Ground054_1K-PNG_NormalDX.png", "Ground054_1K-PNG_Roughness.png"}) {
        touch(dir / "acg" / f);
    }
    const render::TextureSet ground = render::textureSet((dir / "acg").string());
    CHECK(ground.normal == (dir / "acg" / "Ground054_1K-PNG_NormalDX.png").string() && ground.normalDirectX);
    CHECK(!ground.roughness.empty());
    // Ours: normal.png, DirectX's where texture.txt says so.
    touch(dir / "ours" / "color.png");
    touch(dir / "ours" / "normal.png");
    std::ofstream(dir / "ours" / "texture.txt") << "size 2\nnormal dx\n";
    const render::TextureSet ours = render::textureSet((dir / "ours").string());
    CHECK(ours.normal == (dir / "ours" / "normal.png").string() && ours.normalDirectX);
    // None beside it: none.
    touch(dir / "plain" / "wall_diff_1k.png");
    CHECK(render::textureSet((dir / "plain" / "wall_diff_1k.png").string()).normal.empty());
    fs::remove_all(dir);
}

TEST(uv_a_normal_bends_as_its_map_says) {
    render::TexturePicture map;
    map.width = map.height = 1;
    const Vec3 n(0.0f, 1.0f, 0.0f), t(1.0f, 0.0f, 0.0f);  // v along n x t: -z
    auto bent = [&](const Vec3& pixel, bool directX, float handed, float strength) {
        map.pixels = {pixel};
        return render::bentNormal(map, directX, Vec2(0.3f, 0.6f), n, t, handed, strength, n);
    };
    // Flat: as it was.
    CHECK(length(bent(Vec3(0.5f, 0.5f, 1.0f), false, 1.0f, 1.0f) - n) < 1e-6f);
    // 30 degrees toward u: half along the tangent.
    const Vec3 tilt(0.75f, 0.5f, 0.5f + 0.4330127f);
    CHECK(length(bent(tilt, false, 1.0f, 1.0f) - Vec3(0.5f, 0.8660254f, 0.0f)) < 1e-4f);
    // Toward v (green up): along n x t, -z -- or +z with DirectX's green, or
    // with the uv mirrored.
    const Vec3 up(0.5f, 0.75f, 0.5f + 0.4330127f);
    CHECK(length(bent(up, false, 1.0f, 1.0f) - Vec3(0.0f, 0.8660254f, -0.5f)) < 1e-4f);
    CHECK(length(bent(up, true, 1.0f, 1.0f) - Vec3(0.0f, 0.8660254f, 0.5f)) < 1e-4f);
    CHECK(length(bent(up, false, -1.0f, 1.0f) - Vec3(0.0f, 0.8660254f, 0.5f)) < 1e-4f);
    // Half as strong: half the tilt across, up halfway to 1 -- as Cycles has it.
    CHECK(length(bent(tilt, false, 1.0f, 0.5f) - normalize(Vec3(0.25f, 0.9330127f, 0.0f))) < 1e-4f);
    CHECK(length(bent(tilt, false, 1.0f, 0.0f) - n) < 1e-6f);
    // All the way over: no further than just above the surface.
    const Vec3 flat = bent(Vec3(1.0f, 0.5f, 0.5f), false, 1.0f, 1.0f);
    CHECK(dot(flat, n) > 0.04f && flat.x > 0.99f);
}

TEST(uv_the_renderers_lay_a_picture_on_by_uv_and_bend_the_light_by_its_normal_map) {
    // The ground from above under an even sky, a picture of four colours
    // laid on by its uv: each quarter of the picture where the uv puts it
    // -- red at the top left, green at the top right, blue at the bottom
    // left, white at the bottom right.
    const fs::path dir = fs::temp_directory_path() / "pg_test_uv_render";
    fs::remove_all(dir);
    fs::create_directories(dir / "quarters");
    const std::string quarters = (dir / "quarters" / "quarters_diff.png").string();
    writePicture(quarters, 16, 16, [](int x, int y) {
        const bool right = x >= 8, top = y < 8;
        return top ? (right ? Vec3(0.1f, 0.8f, 0.1f) : Vec3(0.8f, 0.1f, 0.1f))
                   : (right ? Vec3(0.8f, 0.8f, 0.8f) : Vec3(0.1f, 0.1f, 0.8f));
    });
    sim::Look even;
    even.lightIntensity = 0.0f;
    even.skyIntensity = 1.0f;
    even.skyColor = Vec3(1.0f);
    even.floor = false;
    render::SceneBuilder builder;
    const auto scene = builder.build(fromAbove(uvGround(quarters), even, 48));
    render::Settings s;
    s.width = s.height = 48;
    s.samples = 16;
    s.denoise = false;
    s.sky = render::Settings::Sky::Look;
    std::vector<std::pair<const char*, render::Image>> pictures = {{"the path tracer", traced(scene, s)}};
    if (render::cyclesAvailable()) pictures.emplace_back("Cycles", cycled(scene, s));
    for (const auto& [who, img] : pictures) {
        // The quarters of the picture of the ground -- the view a little
        // inside its edges.
        const Vec3 tl = meanOf(img, 10, 10, 22, 22), tr = meanOf(img, 26, 10, 38, 22), bl = meanOf(img, 10, 26, 22, 38),
                   br = meanOf(img, 26, 26, 38, 38);
        std::printf("  %s: top left %.2f %.2f %.2f, top right %.2f %.2f %.2f, bottom left %.2f %.2f %.2f, bottom right "
                    "%.2f %.2f %.2f\n",
                    who, tl.x, tl.y, tl.z, tr.x, tr.y, tr.z, bl.x, bl.y, bl.z, br.x, br.y, br.z);
        CHECK(tl.x > 3.0f * tl.y && tl.x > 3.0f * tl.z);
        CHECK(tr.y > 3.0f * tr.x && tr.y > 3.0f * tr.z);
        CHECK(bl.z > 3.0f * bl.x && bl.z > 3.0f * bl.y);
        CHECK(br.x > 0.5f * br.z && br.x > 3.0f * tl.y && std::fabs(br.x - br.z) < 0.2f * br.x);
    }

    // The same ground of grey, lit by a low sun from +x -- its normal map
    // tilting it 30 degrees toward the sun, away from it, or not at all:
    // brighter, darker; and a normal map tilted up the picture (toward -z)
    // under a sun from -z, brighter as OpenGL has it -- darker read as
    // DirectX's, its green the other way.
    sim::Look low;
    low.lightIntensity = 3.0f;
    low.lightElevation = 30.0f;
    low.skyIntensity = 0.0f;
    low.floor = false;
    auto lit = [&](const std::string& name, const Vec3& normal, float azimuth, bool cycles) {
        const fs::path folder = dir / name;
        fs::create_directories(folder);
        writePicture((folder / (name + "_diff.png")).string(), 4, 4, [](int, int) { return Vec3(0.5f); });
        writePicture((folder / (name + (name.find("dx") != std::string::npos ? "_nor_dx.png" : "_nor_gl.png"))).string(),
                     4, 4, [&](int, int) { return normal; });
        sim::Look look = low;
        look.lightAzimuth = azimuth;
        const auto litScene = builder.build(fromAbove(uvGround((folder / (name + "_diff.png")).string()), look, 32));
        render::Settings ls;
        ls.width = ls.height = 32;
        ls.samples = 16;
        ls.denoise = false;
        ls.sky = render::Settings::Sky::Look;
        const render::Image img = cycles ? cycled(litScene, ls) : traced(litScene, ls);
        return meanOf(img, 6, 6, 26, 26).y;
    };
    const float c30 = 0.8660254f, s30 = 0.5f;
    float ratios[2] = {0.0f, 0.0f};
    for (const bool cycles : {false, true}) {
        if (cycles && !render::cyclesAvailable()) continue;
        const char* who = cycles ? "Cycles" : "the path tracer";
        const float flat = lit("flat", Vec3(0.5f, 0.5f, 1.0f), 0.0f, cycles);
        const float toward = lit("toward", Vec3(0.5f + 0.5f * s30, 0.5f, 0.5f + 0.5f * c30), 0.0f, cycles);
        const float away = lit("away", Vec3(0.5f - 0.5f * s30, 0.5f, 0.5f + 0.5f * c30), 0.0f, cycles);
        const Vec3 up(0.5f, 0.5f + 0.5f * s30, 0.5f + 0.5f * c30);
        const float gl = lit("upgl", up, 270.0f, cycles), dx = lit("updx", up, 270.0f, cycles);
        std::printf("  %s: flat %.3f, tilted toward the sun %.3f (x %.2f), away %.3f (x %.2f); up the picture %.3f, "
                    "read as DirectX's %.3f\n",
                    who, flat, toward, toward / flat, away, away / flat, gl, dx);
        // Lambert: sin 30 = 0.5 flat, cos 0 toward, cos 90 away -- and
        // toward, the normal halfway between the sun and the eye above:
        // its highlight too.
        CHECK(toward > 1.5f * flat);
        CHECK(away < 0.35f * flat);
        CHECK(gl > 1.5f * flat && dx < 0.35f * flat);
        ratios[cycles ? 1 : 0] = toward / flat;
    }
    if (ratios[1] > 0.0f) CHECK(std::fabs(ratios[1] / ratios[0] - 1.0f) < 0.05f);
    fs::remove_all(dir);
}

TEST(uv_a_round_surface_bends_the_light_as_cycles_bends_it) {
    // A ball with its uv round it, a normal map tilting it 30 degrees
    // toward u and 20 toward v all over, lit from the side and above: the
    // tangents of its curved surface -- ours and Cycles' (MikkTSpace) --
    // bend it the same way, picture for picture.
    if (!render::cyclesAvailable()) return;
    const fs::path dir = fs::temp_directory_path() / "pg_test_uv_ball";
    fs::remove_all(dir);
    fs::create_directories(dir);
    writePicture((dir / "ball_diff.png").string(), 4, 4, [](int, int) { return Vec3(0.6f); });
    const Vec3 m = normalize(Vec3(0.5f, 0.35f, 0.8f));
    writePicture((dir / "ball_nor_gl.png").string(), 4, 4, [&](int, int) { return m * 0.5f + Vec3(0.5f); });
    // ... and one without a normal map.
    fs::create_directories(dir / "plain");
    writePicture((dir / "plain" / "plain_diff.png").string(), 4, 4, [](int, int) { return Vec3(0.6f); });
    GeometryPtr round = cooked("sphere", nullptr, [](Node& n) {
        n.setFloat("radius", 1.0f);
        n.setInt("rows", 48);
        n.setInt("columns", 96);
    });
    round = cooked("normal", round, [](Node&) {});
    round = cooked("uvproject", round, [](Node& n) { n.setInt("projection", 3); });
    auto ballOf = [&](const fs::path& texture) {
        return cooked("material", round, [&](Node& n) {
            n.setInt("material", 0);
            n.setString("texture", texture.string());
        });
    };
    const GeometryPtr ball = ballOf(dir / "ball_diff.png");
    sim::Look look;
    look.lightIntensity = 3.0f;
    look.lightAzimuth = 30.0f;
    look.lightElevation = 35.0f;
    look.skyIntensity = 0.0f;
    look.floor = false;
    sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 0.0f, 4.5f), Vec3(0.0f));
    cam.width = cam.height = 40;
    render::SceneInput in;
    in.geometry = ball;
    in.look = look;
    in.camera = cam;
    render::SceneBuilder builder;
    const auto scene = builder.build(in);
    render::Settings s;
    s.width = s.height = 40;
    s.samples = 64;
    s.denoise = false;
    s.sky = render::Settings::Sky::Look;
    const render::Image ours = traced(scene, s), theirs = cycled(scene, s);
    // Region by region -- quarters of the ball and its middle.
    const int boxes[][4] = {{8, 8, 20, 20}, {20, 8, 32, 20}, {8, 20, 20, 32}, {20, 20, 32, 32}, {14, 14, 26, 26}};
    float worst = 0.0f;
    for (const auto& b : boxes) {
        const float a = meanOf(ours, b[0], b[1], b[2], b[3]).y, c = meanOf(theirs, b[0], b[1], b[2], b[3]).y;
        std::printf("  (%d %d)-(%d %d): ours %.3f, Cycles' %.3f\n", b[0], b[1], b[2], b[3], a, c);
        worst = std::max(worst, std::fabs(a - c) / std::max(c, 0.02f));
    }
    CHECK(worst < 0.1f);
    // ... and not as it is without its map: the middle twice as bright.
    in.geometry = ballOf(dir / "plain" / "plain_diff.png");
    const render::Image plain = traced(builder.build(in), s);
    const float bentMiddle = meanOf(ours, 14, 14, 26, 26).y, plainMiddle = meanOf(plain, 14, 14, 26, 26).y;
    std::printf("  the middle without the map %.3f, with it %.3f\n", plainMiddle, bentMiddle);
    CHECK(bentMiddle > 1.6f * plainMiddle);
    fs::remove_all(dir);
}
