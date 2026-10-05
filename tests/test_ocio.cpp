//
// OpenColorIO configs (render/Ocio.h) without the library: the YAML they
// are written in (io/Yaml.h), what a config names, and light through its
// views and colour spaces held to what OpenColorIO 2.6 itself makes of the
// same light (tests/data/ocio/make_ocio.py wrote the configs, their tables
// and ocio.txt); and a config's view as the render shows its light.
//
#include "pg/io/Yaml.h"
#include "pg/render/Ocio.h"
#include "pg/render/PathTracer.h"
#include "pg/sim/Network.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace pg;
using namespace pg::render;
namespace fs = std::filesystem;

namespace {

const std::string kData = PG_TEST_DATA_DIR "/ocio/";

/// A line of ocio.txt: what, the config, the names, three in, three out.
struct Reference {
    char what = 'V';
    std::string config;
    std::vector<std::string> names;
    Vec3 in, out;
};

std::vector<Reference> references() {
    std::vector<Reference> out;
    std::ifstream file(kData + "ocio.txt");
    std::string line;
    while (std::getline(file, line)) {
        std::vector<std::string> fields;
        std::string field;
        std::istringstream split(line);
        while (std::getline(split, field, '\t')) fields.push_back(field);
        if (fields.size() < 5) continue;
        Reference r;
        r.what = fields[0][0];
        r.config = fields[1];
        r.names.assign(fields.begin() + 2, fields.end() - 2);
        std::istringstream in(fields[fields.size() - 2]), o(fields.back());
        in >> r.in.x >> r.in.y >> r.in.z;
        o >> r.out.x >> r.out.y >> r.out.z;
        if (in && o) out.push_back(r);
    }
    return out;
}

const OcioConfig& config(const std::string& name) {
    static std::map<std::string, std::shared_ptr<const OcioConfig>> loaded;
    auto& c = loaded[name];
    if (!c) {
        std::string error;
        c = OcioConfig::load(kData + name, error);
        if (!c) {
            std::fprintf(stderr, "%s: %s\n", name.c_str(), error.c_str());
            std::abort();
        }
    }
    return *c;
}

/// How far `got` is from `want`: by the size of the value, at least 1.
float off(const Vec3& got, const Vec3& want) {
    float worst = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float e = std::isfinite(got[k]) ? std::abs(got[k] - want[k]) / std::max(1.0f, std::abs(want[k])) : 1e9f;
        worst = std::max(worst, e);
    }
    return worst;
}

/// Each line of `what` through the processor `make` builds for its names:
/// the worst it is off, and how many lines.
template <class Make>
float worstOf(char what, Make make, int& counted) {
    float worst = 0.0f;
    counted = 0;
    std::string key;
    OcioProcessor processor;
    bool ok = false;
    for (const Reference& r : references()) {
        if (r.what != what) continue;
        std::string now = r.config;
        for (const std::string& n : r.names) now += "\t" + n;
        if (now != key) {
            key = now;
            std::string error;
            ok = make(r, processor, error);
            if (!ok) std::fprintf(stderr, "%s: %s\n", now.c_str(), error.c_str());
            CHECK(ok);
        }
        if (!ok) continue;
        const float e = off(processor.apply(r.in), r.out);
        if (e > worst && e > 1e-4f) {
            std::fprintf(stderr, "  %c %s: (%g %g %g) gives (%g %g %g), OpenColorIO (%g %g %g)\n", what, key.c_str(), r.in.x,
                         r.in.y, r.in.z, processor.apply(r.in).x, processor.apply(r.in).y, processor.apply(r.in).z, r.out.x,
                         r.out.y, r.out.z);
        }
        worst = std::max(worst, e);
        ++counted;
    }
    return worst;
}

std::string temporaryConfig(const std::string& name, const std::string& text) {
    const fs::path dir = fs::temp_directory_path() / "pg_test_ocio";
    fs::create_directories(dir);
    const fs::path path = dir / name;
    std::ofstream(path) << text;
    return path.string();
}

}  // namespace

TEST(yaml_reads_what_opencolorio_configs_write) {
    const std::string text = R"(ocio_profile_version: 2.4   # a comment
name: "quoted \"and\" escaped"
single: 'it''s'
description: |
  Two lines
  of text.
folded: >
  folded
  together
empty:
roles:
  scene_linear: ACEScg
list: [a, b, "c d"]
flow: {matrix: [1, 0, 0,
                0, 1, 0], name: x}
spaces:
  - !<ColorSpace>
    name: one
    to_scene_reference: !<GroupTransform>
      children:
        - !<MatrixTransform> {matrix: [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1]}
        - !<ExponentTransform> {value: 2.2, style: mirror}
  - name: two
    isdata: true
views:
  - !<Views> [A, B]
)";
    io::YamlNode root;
    std::string error;
    CHECK(io::parseYaml(text, root, error));
    CHECK(root.isMapping());
    CHECK(root.text("ocio_profile_version") == "2.4");
    CHECK(root.text("name") == "quoted \"and\" escaped");
    CHECK(root.text("single") == "it's");
    CHECK(root.text("description") == "Two lines\nof text.\n");
    CHECK(root.text("folded") == "folded together\n");
    CHECK(root.find("empty") && root.find("empty")->isNull());
    CHECK(root.find("roles")->text("scene_linear") == "ACEScg");
    const io::YamlNode* list = root.find("list");
    CHECK(list->isSequence() && list->items.size() == 3 && list->items[2].scalar == "c d");
    const io::YamlNode* flow = root.find("flow");
    CHECK(flow->isMapping() && flow->find("matrix")->items.size() == 6 && flow->text("name") == "x");
    const io::YamlNode* spaces = root.find("spaces");
    CHECK(spaces->isSequence() && spaces->items.size() == 2);
    CHECK(spaces->items[0].tag == "ColorSpace" && spaces->items[0].text("name") == "one");
    const io::YamlNode* children = spaces->items[0].find("to_scene_reference")->find("children");
    CHECK(spaces->items[0].find("to_scene_reference")->tag == "GroupTransform");
    CHECK(children->items.size() == 2 && children->items[0].tag == "MatrixTransform");
    CHECK(children->items[0].find("matrix")->items.size() == 16);
    CHECK(children->items[1].text("style") == "mirror");
    CHECK(spaces->items[1].text("name") == "two" && spaces->items[1].text("isdata") == "true");
    CHECK(root.find("views")->items[0].tag == "Views" && root.find("views")->items[0].items.size() == 2);
    // What it cannot read it says, with the line.
    CHECK(!io::parseYaml("a: [1, 2\nb: 3\n", root, error));
    CHECK(!error.empty());
}

TEST(ocio_config_names_its_spaces_displays_and_views) {
    const OcioConfig& c = config("config.ocio");
    // The active displays in their order; the hidden one not.
    const std::vector<std::string> displays = c.displays();
    CHECK(displays == (std::vector<std::string>{"sRGB - Display", "Display P3 - Display", "Rec.1886 Rec.709 - Display"}));
    CHECK(c.defaultDisplay() == "sRGB - Display");
    // A display's views -- its own and those it shares -- in the order of active_views.
    CHECK(c.views("sRGB - Display") ==
          (std::vector<std::string>{"ACES 1.0 - SDR Video", "ACES 2.0 - SDR 100 nits (Rec.709)", "Un-tone-mapped", "Film",
                                    "Graded", "Video", "Raw"}));
    CHECK(c.defaultView("Display P3 - Display") == "ACES 1.0 - SDR Video");
    CHECK(c.display("srgb - display") == "sRGB - Display");
    CHECK(c.display("nowhere").empty());
    // Colour spaces by name in any case, by alias, by role.
    CHECK(c.colorSpace("acescg") == "ACEScg");
    CHECK(c.colorSpace("lin_rec709") == "Linear Rec.709 (sRGB)");
    CHECK(c.colorSpace("scene_linear") == "ACEScg");
    CHECK(c.colorSpace("Non-Color") == "Raw");
    CHECK(c.colorSpace("nothing").empty());
    CHECK(c.role("aces_interchange") == "ACES2065-1");
    CHECK(c.linearRec709() == "Linear Rec.709 (sRGB)");
    CHECK(c.colorSpaces().size() == 22);
    // A config of version 1: its views each a colour space.
    const OcioConfig& v1 = config("config_v1.ocio");
    CHECK(v1.displays() == std::vector<std::string>{"sRGB"});
    CHECK(v1.views("sRGB") == (std::vector<std::string>{"Film", "Log", "Raw"}));
    CHECK(v1.role("scene_linear") == "linear");
}

TEST(ocio_shows_light_as_opencolorio_does) {
    // The ACES outputs, a film's table, looks before them, a view of a
    // display's light to another's, raw data, a config of version 1: on the
    // screen as OpenColorIO shows it, to a hundredth of a step of 255.
    int counted = 0;
    const float worst = worstOf(
        'V',
        [](const Reference& r, OcioProcessor& p, std::string& error) {
            return config(r.config).viewProcessor(r.names[0], r.names[1], r.names[2], r.names[3], p, error);
        },
        counted);
    CHECK(counted == 23 * 14);
    CHECK(worst < 2e-5f);
}

TEST(ocio_takes_pictures_back_as_opencolorio_does) {
    // Each step back as the config takes it back: a colour space's own
    // to_scene_reference where it has one (Blender's way: a table there,
    // another back), else the inverse of the way there.
    int counted = 0;
    const float worst = worstOf(
        'I',
        [](const Reference& r, OcioProcessor& p, std::string& error) {
            return config(r.config).viewProcessor(r.names[0], r.names[1], r.names[2], r.names[3], p, error, true);
        },
        counted);
    CHECK(counted == 23 * 7);
    CHECK(worst < 2e-5f);
}

TEST(ocio_colour_spaces_are_opencolorio_s) {
    // From the scene's light into each colour space and back: logs of
    // cameras and of ACES, exponents, CDLs, ranges, tables in each kind of
    // file, the gamut compression.
    int counted = 0;
    const auto between = [](const Reference& r, OcioProcessor& p, std::string& error) {
        return config(r.config).processor(r.names[0], r.names[1], p, error);
    };
    float worst = worstOf('C', between, counted);
    CHECK(counted == 532);
    CHECK(worst < 2e-6f);
    // Back through a 3D table, as OpenColorIO's processors take it by
    // default: its exact inverse tabled at 48 x 48 x 48 points.
    worst = worstOf('T', between, counted);
    CHECK(counted == 14);
    CHECK(worst < 2e-6f);
}

TEST(ocio_view_shows_a_render) {
    std::string error;
    const auto view = OcioView::make(kData + "config.ocio", "", "", "", "", error);
    CHECK(view != nullptr);
    if (!view) return;
    // The config's first display and its first view; the render's light
    // taken as the config's own linear Rec. 709.
    CHECK(view->display() == "sRGB - Display");
    CHECK(view->view() == "ACES 1.0 - SDR Video");
    CHECK(view->space() == "Linear Rec.709 (sRGB)");
    CHECK(view->invertible());
    OcioProcessor p;
    CHECK(config("config.ocio").viewProcessor("Linear Rec.709 (sRGB)", "sRGB - Display", "ACES 1.0 - SDR Video", "", p, error));
    for (const Vec3 light : {Vec3(0.18f), Vec3(1.0f, 0.5f, 0.1f), Vec3(4.0f, 0.2f, 0.05f)}) {
        CHECK(off(view->shown(light), p.apply(light)) == 0.0f);
    }
    // The same arguments, the same file: the view made before.
    CHECK(OcioView::make(kData + "config.ocio", "", "", "", "", error) == view);
    // A picture back to light that shows as it: exactly, where OpenColorIO's
    // inverse is only near (the red modifier's).
    float worst = 0.0f;
    for (float r = 0.05f; r < 1.0f; r += 0.15f) {
        for (float g = 0.05f; g < 1.0f; g += 0.15f) {
            for (float b = 0.05f; b < 1.0f; b += 0.15f) {
                const Vec3 picture(r, g, b);
                worst = std::max(worst, off(view->shown(view->unshown(picture)), picture));
            }
        }
    }
    CHECK(worst < 0.5f / 255.0f);
    // Another display, another view, looks before it.
    const auto p3 = OcioView::make(kData + "config.ocio", "display p3 - display", "Un-tone-mapped", "Warm, -Contrast", "", error);
    CHECK(p3 && p3->display() == "Display P3 - Display" && p3->view() == "Un-tone-mapped");
    // What is not there, it says.
    CHECK(!OcioView::make(kData + "config.ocio", "", "Nope", "", "", error));
    CHECK(error.find("no view Nope") != std::string::npos);
    CHECK(!OcioView::make(kData + "config.ocio", "Nowhere", "", "", "", error));
    CHECK(error.find("no display Nowhere") != std::string::npos);
    CHECK(!OcioView::make(kData + "config.ocio", "", "", "", "Nothing", error));
    CHECK(error.find("no colour space Nothing") != std::string::npos);
    CHECK(!OcioView::make(kData + "missing.ocio", "", "", "", "", error));
    CHECK(error.find("cannot read") != std::string::npos);
}

TEST(ocio_view_takes_render_light_through_aces2065_1) {
    // A config with no linear Rec. 709 of its own: the light through
    // ACES2065-1, its aces_interchange role.
    const std::string path = temporaryConfig("interchange.ocio", R"(ocio_profile_version: 2
roles:
  aces_interchange: ACES2065-1
  scene_linear: ACEScg
displays:
  sRGB:
    - !<View> {name: Video, view_transform: Video, display_colorspace: sRGB}
view_transforms:
  - !<ViewTransform>
    name: Video
    from_scene_reference: !<BuiltinTransform> {style: ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-VIDEO_1.0}
display_colorspaces:
  - !<ColorSpace>
    name: sRGB
    from_display_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_sRGB}
colorspaces:
  - !<ColorSpace>
    name: ACES2065-1
  - !<ColorSpace>
    name: ACEScg
    to_scene_reference: !<BuiltinTransform> {style: ACEScg_to_ACES2065-1}
)");
    std::string error;
    const auto view = OcioView::make(path, "", "", "", "", error);
    CHECK(view != nullptr);
    if (!view) return;
    CHECK(view->space() == "ACES2065-1");
    // As the ACES config shows linear Rec. 709 light through the same view.
    const auto aces = OcioView::make(kData + "config.ocio", "", "ACES 1.0 - SDR Video", "", "", error);
    CHECK(aces != nullptr);
    for (const Vec3 light : {Vec3(0.18f), Vec3(1.0f, 0.5f, 0.1f), Vec3(0.02f, 0.3f, 2.0f)}) {
        CHECK(off(view->shown(light), aces->shown(light)) < 1e-5f);
    }
}

TEST(ocio_says_what_it_cannot_do) {
    std::string error;
    const auto c = OcioConfig::parse(R"(ocio_profile_version: 2
roles: {scene_linear: linear}
displays:
  sRGB:
    - !<View> {name: Graded, colorspace: srgb, looks: grade}
    - !<View> {name: HDR, colorspace: pq}
    - !<View> {name: Table, colorspace: tabled}
looks:
  - !<Look>
    name: grade
    process_space: linear
    transform: !<GradingPrimaryTransform> {style: log, contrast: {rgb: [1, 1, 1], master: 1.2}}
colorspaces:
  - !<ColorSpace> {name: linear}
  - !<ColorSpace>
    name: srgb
    from_scene_reference: !<ExponentWithLinearTransform> {gamma: 2.4, offset: 0.055, direction: inverse}
  - !<ColorSpace>
    name: pq
    from_scene_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_REC.2100-PQ}
  - !<ColorSpace>
    name: tabled
    from_scene_reference: !<FileTransform> {src: missing.cube}
)",
                                     kData, error);
    CHECK(c != nullptr);
    if (!c) return;
    OcioProcessor p;
    CHECK(!c->viewProcessor("linear", "sRGB", "Graded", "", p, error));
    CHECK(error.find("GradingPrimaryTransform") != std::string::npos);
    CHECK(!c->viewProcessor("linear", "sRGB", "HDR", "", p, error));
    CHECK(error.find("REC.2100-PQ") != std::string::npos);
    CHECK(!c->viewProcessor("linear", "sRGB", "Table", "", p, error));
    CHECK(error.find("missing.cube") != std::string::npos);
    // Not a config at all.
    CHECK(!OcioConfig::parse("name: something\n", kData, error));
    CHECK(error.find("ocio_profile_version") != std::string::npos);
}

TEST(ocio_view_is_what_the_renderers_show) {
    std::string error;
    const auto view = OcioView::make(kData + "config.ocio", "", "ACES 2.0 - SDR 100 nits (Rec.709)", "", "", error);
    CHECK(view != nullptr);
    if (!view) return;
    using V = Settings::View;
    // Shown as the config's view shows it, held to 0 to 1.
    for (const Vec3 light : {Vec3(0.18f), Vec3(1.0f, 0.5f, 0.1f), Vec3(40.0f, 2.0f, 0.5f)}) {
        const Vec3 want = glm::clamp(view->shown(light), 0.0f, 1.0f);
        CHECK(off(shown(light, V::Ocio, view.get()), want) == 0.0f);
    }
    // No config: AgX Punchy, as the network warns.
    CHECK(off(shown(Vec3(0.3f, 0.2f, 0.1f), V::Ocio, nullptr), shown(Vec3(0.3f, 0.2f, 0.1f), V::AgXPunchy)) == 0.0f);
    // A picture: each pixel so, times the exposure.
    Image image;
    image.width = 2;
    image.height = 1;
    image.pixels = {0.1f, 0.2f, 0.3f, 2.0f, 1.0f, 0.5f};
    const std::vector<uint8_t> rgba = toDisplay(image, 2.0f, V::Ocio, view.get());
    const Vec3 second = glm::clamp(view->shown(Vec3(4.0f, 2.0f, 1.0f)), 0.0f, 1.0f);
    CHECK(rgba.size() == 8 && rgba[4] == static_cast<uint8_t>(std::lround(second.x * 255.0f)));
    // A plate's colours back to the light that shows as them.
    for (const Vec3 picture : {Vec3(0.5f), Vec3(0.8f, 0.4f, 0.2f), Vec3(0.1f, 0.3f, 0.6f)}) {
        CHECK(off(shown(unshown(picture, V::Ocio, view.get()), V::Ocio, view.get()), picture) < 0.5f / 255.0f);
    }
}

TEST(ocio_output_node_reads_its_config) {
    sim::Network net;
    const int out = net.add("output");
    net.setParam(out, "render_view", "ocio");
    CHECK(net.setText(out, "render_ocio_config", kData + "config.ocio"));
    CHECK(net.setText(out, "render_ocio_view", "Film"));
    sim::Compiled c = net.compile();
    CHECK(c.render.view == Settings::View::Ocio);
    CHECK(c.render.ocio != nullptr);
    if (c.render.ocio) CHECK(c.render.ocio->view() == "Film" && c.render.ocio->display() == "sRGB - Display");
    // A view it has not: it says so, and AgX Punchy shows the light.
    CHECK(net.setText(out, "render_ocio_view", "Nope"));
    c = net.compile();
    CHECK(c.render.ocio == nullptr);
    bool said = false;
    for (const sim::Problem& p : c.problems) said = said || p.message.find("no view Nope") != std::string::npos;
    CHECK(said);
    // No config at all.
    CHECK(net.setText(out, "render_ocio_config", ""));
    said = false;
    for (const sim::Problem& p : net.compile().problems) said = said || p.message.find("no OCIO Config") != std::string::npos;
    CHECK(said);
}
