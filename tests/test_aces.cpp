//
// ACES (render/Aces.h): the two output transforms, and the colour spaces
// (core/ColorSpace.h), held to what OpenColorIO 2.6's own ACES configs give
// for the same light and pictures (tests/data/aces/make_aces.py wrote
// aces.txt).
//
#include "pg/core/ColorSpace.h"
#include "pg/io/Exr.h"
#include "pg/io/Picture.h"
#include "pg/render/Aces.h"
#include "pg/render/PathTracer.h"
#include "pg/render/Save.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace pg;
using namespace pg::render;
namespace fs = std::filesystem;

namespace {

/// A line of aces.txt: the view or the space, the way, three in, three out.
struct Reference {
    std::string what;
    char way = 'f';
    Vec3 in, out;
};

std::vector<Reference> references() {
    std::vector<Reference> out;
    std::ifstream file(PG_TEST_DATA_DIR "/aces/aces.txt");
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream in(line);
        Reference r;
        in >> r.what >> r.way >> r.in.x >> r.in.y >> r.in.z >> r.out.x >> r.out.y >> r.out.z;
        if (in) out.push_back(r);
    }
    return out;
}

float largest(const Vec3& v) { return std::max(std::abs(v.x), std::max(std::abs(v.y), std::abs(v.z))); }

}  // namespace

TEST(aces_shows_light_as_opencolorio_does) {
    // Greys from deep shadow to a thousand times white, primaries and the
    // colours between them, skin, sky, fire, and colours at random: on the
    // screen, as OpenColorIO shows them, to a hundredth of a step of 255.
    int counted[2] = {0, 0};
    float worst[2] = {0.0f, 0.0f};
    for (const Reference& r : references()) {
        if (r.way != 'f' || (r.what != "1" && r.what != "2")) continue;
        const int v = r.what == "1" ? 0 : 1;
        const Vec3 got = acesShown(r.in, v == 0 ? AcesOutput::V1 : AcesOutput::V2);
        const Vec3 want = glm::clamp(r.out, 0.0f, 1.0f);
        worst[v] = std::max(worst[v], largest(got - want));
        ++counted[v];
    }
    CHECK(counted[0] > 700 && counted[1] > 700);
    CHECK(worst[0] < 1e-4f);
    CHECK(worst[1] < 1e-4f);

    // Middle grey a little below the middle of the screen, white far below
    // its top -- ACES leaves room for the light beyond it -- and the
    // brightest light white.
    for (const AcesOutput o : {AcesOutput::V1, AcesOutput::V2}) {
        const Vec3 grey = acesShown(Vec3(0.18f), o), white = acesShown(Vec3(1.0f), o);
        CHECK(grey.x > 0.3f && grey.x < 0.5f);
        CHECK(white.x > 0.7f && white.x < 0.9f);
        CHECK(acesShown(Vec3(1000.0f), o).x > 0.99f);
        CHECK(acesShown(Vec3(0.0f), o).x < 1e-3f);
    }
}

TEST(aces_takes_a_picture_back_to_the_light_that_shows_as_it) {
    // A plate's pixels go back to light and are shown again: within a
    // quarter of a step of 255 wherever some light shows as the pixel --
    // ACES 1.0 too, whose inverse in OpenColorIO comes back two steps off
    // a saturated red. ACES 2.0's light is OpenColorIO's.
    int counted = 0;
    float worstTrip = 0.0f, worstLight[2] = {0.0f, 0.0f};
    for (const Reference& r : references()) {
        if (r.way != 'i' || (r.what != "1" && r.what != "2")) continue;
        const int v = r.what == "1" ? 0 : 1;
        const AcesOutput o = v == 0 ? AcesOutput::V1 : AcesOutput::V2;
        const Vec3 light = acesUnshown(r.in, o);
        CHECK(std::isfinite(light.x) && std::isfinite(light.y) && std::isfinite(light.z));
        CHECK(light.x >= 0.0f && light.y >= 0.0f && light.z >= 0.0f);
        if (r.out.x < 0.0f || r.out.y < 0.0f || r.out.z < 0.0f) continue;  // no light shows as it
        ++counted;
        worstTrip = std::max(worstTrip, largest(acesShown(light, o) - r.in));
        const Vec3 off = light - r.out;
        worstLight[v] = std::max(worstLight[v], largest(off) / std::max(1.0f, largest(r.out)));
    }
    CHECK(counted > 1000);
    CHECK(worstTrip < 0.25f / 255.0f);
    CHECK(worstLight[0] < 0.03f);
    CHECK(worstLight[1] < 2e-3f);
}

TEST(aces_colour_spaces_are_opencolorio_s) {
    int counted = 0;
    for (const Reference& r : references()) {
        if (r.what != "cg" && r.what != "ap0") continue;
        const LinearSpace space = r.what == "cg" ? LinearSpace::ACEScg : LinearSpace::ACES2065_1;
        const float scale = std::max(1.0f, largest(r.out));
        CHECK(largest(fromRec709(r.in, space) - r.out) < 2e-6f * scale);
        CHECK(largest(toRec709(r.out, space) - r.in) < 2e-6f * scale);
        ++counted;
    }
    CHECK(counted > 200);
    // A picture's chromaticities say which: ACEScg's read back as ACEScg.
    const Mat3 m = toRec709From(chromaticitiesOf(LinearSpace::ACEScg));
    const Vec3 c(0.3f, 0.6f, 0.1f);
    CHECK(largest(m * c - toRec709(c, LinearSpace::ACEScg)) < 1e-6f);
    CHECK(largest(toRec709From(chromaticitiesOf(LinearSpace::Rec709)) * c - c) < 1e-6f);
    CHECK(chromaticitiesOf(LinearSpace::ACES2065_1)[0] == 0.7347f);
}

TEST(aces_views_are_what_the_renderers_show) {
    // The Output's views ACES 1.0 and 2.0 are the transforms above, after
    // the exposure; Standard is sRGB, white and brighter clipped -- and each
    // goes back for a plate.
    using View = Settings::View;
    const Vec3 light(0.4f, 0.2f, 0.05f);
    CHECK(largest(shown(light, View::Aces1) - acesShown(light, AcesOutput::V1)) == 0.0f);
    CHECK(largest(shown(light, View::Aces2) - acesShown(light, AcesOutput::V2)) == 0.0f);
    CHECK(std::abs(shown(Vec3(0.18f), View::Standard).x - srgbEncoded(0.18f)) < 1e-6f);
    CHECK(shown(Vec3(4.0f), View::Standard).x == 1.0f);
    Image image;
    image.width = 1;
    image.height = 1;
    image.pixels = {light.x, light.y, light.z};
    const std::vector<uint8_t> rgba = toDisplay(image, 2.0f, View::Aces2);
    const Vec3 twice = acesShown(light * 2.0f, AcesOutput::V2);
    CHECK(rgba[0] == static_cast<uint8_t>(std::lround(twice.x * 255.0f)));
    CHECK(rgba[1] == static_cast<uint8_t>(std::lround(twice.y * 255.0f)));
    for (const View v : {View::Aces1, View::Aces2, View::Standard}) {
        const Vec3 picture(0.6f, 0.45f, 0.3f);
        CHECK(largest(shown(unshown(picture, v), v) - picture) < 0.5f / 255.0f);
    }
}

TEST(aces_exr_says_and_reads_the_space_of_its_light) {
    // A render's EXR in ACEScg: its light in AP1, its chromaticities AP1's.
    // Read back as a picture -- a plate, a sky -- it is linear Rec. 709 again.
    const fs::path dir = fs::temp_directory_path() / ("pg_test_aces_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    Rendered r;
    r.beauty.width = 2;
    r.beauty.height = 1;
    r.beauty.pixels = {0.8f, 0.3f, 0.1f, 0.05f, 0.4f, 2.5f};
    r.space = LinearSpace::ACEScg;
    std::string error;
    const std::string path = (dir / "cg.exr").string();
    CHECK(savePicture(r, path, "", error));
    io::ExrImage exr;
    CHECK(io::readExr(path, exr, error));
    CHECK(exr.hasChromaticities);
    CHECK(exr.chromaticities == chromaticitiesOf(LinearSpace::ACEScg));
    auto value = [&](const char* name, size_t p) {
        for (const io::ExrChannel& c : exr.channels) {
            if (c.name == name) return c.values[p];
        }
        return -1.0f;
    };
    for (size_t p = 0; p < 2; ++p) {
        const Vec3 cg = fromRec709(Vec3(r.beauty.pixels[3 * p], r.beauty.pixels[3 * p + 1], r.beauty.pixels[3 * p + 2]),
                                   LinearSpace::ACEScg);
        const Vec3 written(value("R", p), value("G", p), value("B", p));
        CHECK(largest(written - cg) < 2e-3f * std::max(1.0f, largest(cg)));
    }
    io::Picture picture;
    CHECK(io::readPicture(path, picture, error));
    for (size_t p = 0; p < 2; ++p) {
        for (size_t c = 0; c < 3; ++c) {
            const float want = r.beauty.pixels[3 * p + c];
            CHECK(std::abs(picture.rgba[4 * p + c] - want) < 3e-3f * std::max(1.0f, want));
        }
    }
    // Without the attribute -- or with Rec. 709's -- as it is.
    r.space = LinearSpace::Rec709;
    CHECK(savePicture(r, path, "", error));
    CHECK(io::readExr(path, exr, error));
    CHECK(exr.hasChromaticities && isRec709(exr.chromaticities));
    CHECK(io::readPicture(path, picture, error));
    CHECK(std::abs(picture.rgba[0] - 0.8f) < 1e-3f);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
