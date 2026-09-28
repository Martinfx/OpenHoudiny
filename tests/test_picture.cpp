// Pictures in (src/pg/io/Picture.h): PNG of every colour type, bit depth
// and filter, interlaced too, to the value; JPEG as libjpeg itself reads
// it (baseline, progressive, 4:2:0, 4:2:2, 4:4:0, 4:4:4, grey, restart
// markers); OpenEXR of each compression as the OpenEXR library writes and
// reads it. The files in tests/data/pictures come from those libraries
// (make_pictures.py). Frame numbers in names, and broken files refused
// without a crash. Pictures out, read back. A plate on a camera and the
// mattes over it, as a network compiles them.
#include "pg/core/Half.h"
#include "pg/io/Exr.h"
#include "pg/io/Inflate.h"
#include "pg/io/Picture.h"
#include "pg/sim/Network.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <tuple>
#include <vector>

using namespace pg;
namespace fs = std::filesystem;

namespace {

const std::string kPictures = std::string(PG_TEST_DATA_DIR) + "/pictures/";

std::vector<uint8_t> bytesOf(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

io::Picture picture(const std::string& name) {
    io::Picture p;
    std::string error;
    if (!io::readPicture(kPictures + name, p, error)) ::testing::fail(__FILE__, __LINE__, name + ": " + error);
    return p;
}

io::ExrImage exr(const std::string& name) {
    io::ExrImage image;
    std::string error;
    if (!io::readExr(kPictures + name, image, error)) ::testing::fail(__FILE__, __LINE__, name + ": " + error);
    return image;
}

/// The largest difference, in 1/255, between a decoded JPEG and what
/// libjpeg decodes it to.
int jpegDifference(const std::string& name) {
    const io::Picture p = picture(name + ".jpg");
    const std::vector<uint8_t> ref = bytesOf(kPictures + name + ".rgb");
    if (ref.size() != static_cast<size_t>(p.width) * static_cast<size_t>(p.height) * 3) return 1000;
    int worst = 0;
    for (size_t i = 0; i < ref.size(); ++i) {
        const int ours = static_cast<int>(std::lround(p.rgba[(i / 3) * 4 + i % 3] * 255.0f));
        worst = std::max(worst, std::abs(ours - ref[i]));
    }
    return worst;
}

struct TempFolder {
    fs::path path;
    explicit TempFolder(const std::string& name) {
        std::random_device rd;
        path = fs::temp_directory_path() / ("pg_test_" + name + "_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string operator/(const std::string& name) const { return (path / name).string(); }
    std::string write(const std::string& name, const std::string& text) const {
        const std::string p = *this / name;
        fs::create_directories(fs::path(p).parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }
};

bool said(const sim::Compiled& c, const std::string& what) {
    for (const sim::Problem& p : c.problems) {
        if (p.message.find(what) != std::string::npos) return true;
    }
    return false;
}

/// Whether two EXRs hold the same channels, value for value (bit for bit).
bool sameExr(const io::ExrImage& a, const io::ExrImage& b) {
    if (a.width != b.width || a.height != b.height || a.channels.size() != b.channels.size()) return false;
    for (size_t c = 0; c < a.channels.size(); ++c) {
        if (a.channels[c].name != b.channels[c].name || a.channels[c].values.size() != b.channels[c].values.size()) return false;
        if (std::memcmp(a.channels[c].values.data(), b.channels[c].values.data(), a.channels[c].values.size() * sizeof(float)) != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(png_reads_every_colour_type_depth_and_filter_to_the_value) {
    // The formulas make_pictures.py wrote the files from.
    const int w = 19, h = 13;
    for (const char* name : {"rgb8.png", "rgb8_adam7.png"}) {
        const io::Picture p = picture(name);
        CHECK_EQ(p.width, w);
        CHECK_EQ(p.height, h);
        CHECK(!p.linear);
        bool same = true;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const int v = (x * 37 + y * 11 + c * 71 + (x * y) % 13) % 256;
                    same = same && p.pixel(x, y)[c] == v / 255.0f;
                }
                same = same && p.pixel(x, y)[3] == 1.0f;
            }
        }
        CHECK(same);
    }
    {
        const io::Picture p = picture("rgba16.png");
        bool same = true;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const int v = (x * 9001 + y * 577 + c * 12345 + x * y * 31) % 65536;
                    same = same && p.pixel(x, y)[c] == static_cast<float>(v) / 65535.0f;
                }
            }
        }
        CHECK(same);
    }
    {
        const io::Picture four = picture("grey4.png");
        const io::Picture one = picture("grey1_adam7.png");
        const io::Picture ga = picture("grey_alpha8.png");
        bool same = true;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                same = same && four.pixel(x, y)[1] == static_cast<float>((x + 3 * y) % 16) / 15.0f;
                same = same && one.pixel(x, y)[2] == static_cast<float>((x + 3 * y) % 2);
                same = same && ga.pixel(x, y)[0] == static_cast<float>((x * 5 + y * 3) % 256) / 255.0f;
                same = same && ga.pixel(x, y)[3] == static_cast<float>((x * y + 7) % 256) / 255.0f;
            }
        }
        CHECK(same);
    }
    {
        const io::Picture p = picture("palette2.png");
        const io::Picture bits = picture("palette_bits.png");
        bool same = true;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int i = (x * 3 + y) % 17;
                same = same && p.pixel(x, y)[0] == static_cast<float>((i * 15) % 256) / 255.0f;
                same = same && p.pixel(x, y)[1] == static_cast<float>((255 - i * 13) % 256) / 255.0f;
                same = same && p.pixel(x, y)[3] == static_cast<float>((i * 29) % 256) / 255.0f;
                const int j = (x * 3 + y) % 16;
                same = same && bits.pixel(x, y)[2] == static_cast<float>((j * 47) % 256) / 255.0f;
                same = same && bits.pixel(x, y)[3] == 1.0f;
            }
        }
        CHECK(same);
    }
    // Stored deflate blocks (what the renderer's own PNG writer makes) read back.
    std::vector<uint8_t> data(70000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 13 + i / 7);
    std::vector<uint8_t> stream = {0x78, 0x01};
    for (size_t at = 0; at < data.size(); at += 65535) {
        const size_t n = std::min<size_t>(65535, data.size() - at);
        stream.push_back(at + n == data.size() ? 1 : 0);
        stream.push_back(static_cast<uint8_t>(n & 0xFF));
        stream.push_back(static_cast<uint8_t>(n >> 8));
        stream.push_back(static_cast<uint8_t>(~n & 0xFF));
        stream.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        stream.insert(stream.end(), data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n));
    }
    uint32_t a = 1, b = 0;
    for (const uint8_t v : data) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t sum = (b << 16) | a;
    for (int i = 3; i >= 0; --i) stream.push_back(static_cast<uint8_t>(sum >> (8 * i)));
    std::vector<uint8_t> back;
    std::string error;
    CHECK(io::zlibInflate(stream, back, data.size(), error));
    CHECK(back == data);
    stream[stream.size() - 1] ^= 1;  // the checksum no longer matches
    back.clear();
    CHECK(!io::zlibInflate(stream, back, data.size(), error));
}

TEST(jpeg_reads_as_libjpeg_does) {
    for (const char* name : {"420", "444", "422", "440", "grey", "progressive", "progressive444", "restart"}) {
        const int worst = jpegDifference(name);
        if (worst != 0) ::testing::fail(__FILE__, __LINE__, std::string(name) + ".jpg differs from libjpeg by " + std::to_string(worst));
    }
    const io::Picture p = picture("420.jpg");
    CHECK_EQ(p.width, 37);
    CHECK_EQ(p.height, 29);
    CHECK(!p.linear);
}

TEST(exr_reads_each_compression_as_the_library_does) {
    const io::ExrImage none = exr("none.exr");
    CHECK_EQ(none.width, 29);
    CHECK_EQ(none.height, 37);
    CHECK_EQ(none.channels.size(), 5u);
    CHECK_EQ(none.channels[0].name, std::string("A"));
    CHECK(!none.channels[0].half);  // A: float
    CHECK(none.channels[1].half);   // B: half
    // Lossless: the same values, bit for bit.
    for (const char* name : {"rle.exr", "zips.exr", "zip.exr", "piz.exr"}) {
        if (!sameExr(exr(name), none)) ::testing::fail(__FILE__, __LINE__, std::string(name) + " differs from none.exr");
    }
    // Lossy: what the library itself reads them to.
    for (const char* name : {"pxr24", "b44", "b44a"}) {
        if (!sameExr(exr(std::string(name) + ".exr"), exr(std::string(name) + "_ref.exr"))) {
            ::testing::fail(__FILE__, __LINE__, std::string(name) + ".exr differs from what OpenEXR reads");
        }
    }
    // Values beyond 1, below 0; integers.
    const io::ExrImage& n = none;
    const auto at = [&](const char* channel, int x, int y) {
        for (const io::ExrChannel& c : n.channels) {
            if (c.name == channel) return c.values[static_cast<size_t>(y) * static_cast<size_t>(n.width) + static_cast<size_t>(x)];
        }
        return -1.0f;
    };
    CHECK_NEAR(at("R", 6, 20), floatFromHalf(halfFromFloat(static_cast<float>(std::sin(6 / 4.0) * 3 + 20 / 10.0))), 0);
    CHECK_NEAR(at("Z", 7, 11), 7011.0f, 0);

    // The data window where it lies in the display window; the rest 0.
    const io::ExrImage window = exr("window.exr");
    CHECK_EQ(window.width, 34);
    CHECK_EQ(window.height, 30);
    const auto value = [&](const io::ExrImage& image, int x, int y) { return image.channels.back().values[static_cast<size_t>(y) * 34 + static_cast<size_t>(x)]; };
    CHECK_NEAR(value(window, 2, 10), 0.0f, 0);
    CHECK_NEAR(value(window, 3, 5), at("R", 3, 5), 0);
    CHECK_NEAR(value(window, 26, 24), at("R", 26, 24), 0);

    // As a picture: RGBA, linear; grey from Y.
    const io::Picture rgba = picture("none.exr");
    CHECK(rgba.linear);
    CHECK_NEAR(rgba.pixel(6, 20)[0], at("R", 6, 20), 0);
    CHECK_NEAR(rgba.pixel(6, 20)[3], at("A", 6, 20), 0);
    const io::Picture grey = picture("luminance.exr");
    CHECK_NEAR(grey.pixel(6, 20)[1], at("R", 6, 20), 0);

    // What is not read says so.
    io::Picture p;
    std::string error;
    CHECK(!io::readPicture(kPictures + "dwaa.exr", p, error));
    CHECK(error.find("DWA") != std::string::npos);
}

TEST(sequence_names_give_each_frame_its_file) {
    CHECK_EQ(io::sequenceFile("plate.####.exr", 1001), std::string("plate.1001.exr"));
    CHECK_EQ(io::sequenceFile("plate.######.exr", 7), std::string("plate.000007.exr"));
    CHECK_EQ(io::sequenceFile("plate.$F4.jpg", 12), std::string("plate.0012.jpg"));
    CHECK_EQ(io::sequenceFile("plate.$F.jpg", 12), std::string("plate.12.jpg"));
    CHECK_EQ(io::sequenceFile("plate_%04d.png", 3), std::string("plate_0003.png"));
    CHECK_EQ(io::sequenceFile("plate_%d.png", 3), std::string("plate_3.png"));
    CHECK_EQ(io::sequenceFile("still.png", 3), std::string("still.png"));
    CHECK(io::isSequence("a.####.exr"));
    CHECK(!io::isSequence("still.png"));
}

TEST(broken_pictures_are_refused_not_crashed_on) {
    std::mt19937 rng(11);
    int files = 0;
    for (const auto& entry : fs::directory_iterator(kPictures)) {
        const std::string ext = entry.path().extension().string();
        if (ext != ".png" && ext != ".jpg" && ext != ".exr") continue;
        const std::vector<uint8_t> good = bytesOf(entry.path().string());
        for (int k = 0; k < 40; ++k, ++files) {
            std::vector<uint8_t> bad = good;
            if (k % 4 == 0) {
                bad.resize(std::uniform_int_distribution<size_t>(0, good.size())(rng));
            } else {
                const int changes = 1 + static_cast<int>(rng() % 8);
                for (int i = 0; i < changes && !bad.empty(); ++i) {
                    bad[std::uniform_int_distribution<size_t>(0, bad.size() - 1)(rng)] = static_cast<uint8_t>(rng());
                }
            }
            io::Picture p;
            std::string error;
            if (io::decodePicture(bad, p, error)) {
                CHECK(p.rgba.size() == static_cast<size_t>(p.width) * static_cast<size_t>(p.height) * 4);
            } else {
                CHECK(!error.empty());
            }
        }
    }
    CHECK(files > 1000);
    // Deflate that grows past what it should is stopped.
    std::vector<uint8_t> out;
    std::string error;
    const std::vector<uint8_t> zeros = {0x78, 0x9C, 0x63, 0x60, 0x18, 0x05, 0xA3, 0x60, 0x14, 0x0C, 0x77, 0x00, 0x00};
    CHECK(!io::zlibInflate(zeros, out, 10, error));
}

TEST(pictures_written_read_back) {
    // PNG: the bytes as they were -- in one stored block, or in many.
    for (const auto& [w, h, channels] : std::vector<std::tuple<int, int, int>>{{1, 1, 3}, {7, 5, 4}, {300, 90, 4}, {257, 300, 3}}) {
        std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(channels));
        for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i * 31 + i / 7);
        const std::string png = io::encodePng(pixels.data(), w, h, channels);
        io::Picture p;
        std::string error;
        CHECK(io::decodePng({reinterpret_cast<const uint8_t*>(png.data()), png.size()}, p, error));
        CHECK_EQ(p.width, w);
        CHECK_EQ(p.height, h);
        bool same = true;
        for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i) {
            for (int c = 0; c < 4; ++c) {
                const int want = c < channels ? pixels[i * static_cast<size_t>(channels) + static_cast<size_t>(c)] : 255;
                same = same && std::lround(p.rgba[i * 4 + static_cast<size_t>(c)] * 255.0f) == want;
            }
        }
        CHECK(same);
    }
    const uint8_t one[4] = {1, 2, 3, 4};
    CHECK(io::encodePng(nullptr, 1, 1, 3).empty());
    CHECK(io::encodePng(one, 0, 1, 3).empty());
    CHECK(io::encodePng(one, 1, 1, 2).empty());

    // To files, the kind their names say.
    TempFolder dir("written");
    io::Picture in;
    in.width = 13;
    in.height = 9;
    in.rgba.resize(13 * 9 * 4);
    for (size_t i = 0; i < in.rgba.size(); ++i) in.rgba[i] = i % 4 == 3 ? 1.0f : static_cast<float>(i % 37) / 20.0f - 0.3f;
    std::string error;
    io::Picture back;
    CHECK(io::writePicture(dir / "a.png", in, 90, error));
    CHECK(io::readPicture(dir / "a.png", back, error));
    CHECK(!back.linear);
    bool rounded = back.width == 13 && back.height == 9;
    for (size_t i = 0; rounded && i < in.rgba.size(); ++i) {
        rounded = std::lround(back.rgba[i] * 255.0f) == std::lround(std::clamp(in.rgba[i], 0.0f, 1.0f) * 255.0f);
    }
    CHECK(rounded);
    CHECK(io::writePicture(dir / "a.exr", in, 90, error));
    CHECK(io::readPicture(dir / "a.exr", back, error));
    CHECK(back.linear);
    bool halves = back.width == 13 && back.height == 9;
    for (size_t i = 0; halves && i < in.rgba.size(); ++i) halves = back.rgba[i] == floatFromHalf(halfFromFloat(in.rgba[i]));
    CHECK(halves);
    CHECK(io::writePicture(dir / "a.JPG", in, 100, error));
    CHECK(io::readPicture(dir / "a.JPG", back, error));
    CHECK_EQ(back.width, 13);
    // What is not written says why.
    CHECK(!io::writePicture(dir / "a.tif", in, 90, error));
    CHECK(error.find(".tif") != std::string::npos || error.find("written as") != std::string::npos);
    CHECK(!io::writePicture(dir / "a.png", io::Picture{}, 90, error));
    CHECK(!io::writePicture(dir / "no/such/folder/a.png", in, 90, error));
}

TEST(a_plate_and_its_mattes_compile_into_the_shot) {
    TempFolder dir("plate");
    sim::Network net;
    const int cam = net.add("camera");
    const int out = net.add("output");
    const int wall = net.add("object");
    CHECK(net.connect(cam, "camera", out, "camera"));
    CHECK(net.setText(cam, "plate", "plates/shot.####.jpg"));
    net.setParam(cam, "plate_frame", "1001");
    CHECK(net.setParam(wall, "matte", "catcher"));

    // The plate, read from the network's folder; its frames from 1001.
    sim::Compiled c = net.compile(dir.path.string());
    const std::string plate = (dir.path / "plates" / "shot.####.jpg").lexically_normal().string();
    CHECK_EQ(c.camera.plate, plate);
    CHECK_EQ(c.camera.plateFile(1), io::sequenceFile(plate, 1001));
    CHECK_EQ(c.camera.plateFile(3), io::sequenceFile(plate, 1003));
    CHECK(said(c, "no plate at frame 1"));
    dir.write("plates/shot.1001.jpg", "not looked into yet");
    CHECK(!said(net.compile(dir.path.string()), "no plate at frame 1"));
    // An object over it, and the floor: the ground it was filmed on unless said.
    CHECK(c.solids.size() == 1 && c.solids[0].matte == sim::Matte::Catcher);
    CHECK(c.look.floorMatte == sim::Matte::Catcher);
    CHECK(net.setParam(out, "floor_matte", "holdout"));
    CHECK(net.setParam(wall, "matte", "holdout"));
    c = net.compile(dir.path.string());
    CHECK(c.look.floorMatte == sim::Matte::Holdout);
    CHECK(c.solids[0].matte == sim::Matte::Holdout);
    // A network from before plates: its objects are themselves, its floor the ground.
    sim::Network old;
    std::string error;
    CHECK(sim::Network::load("pgsim 1\nnode 1 object 1 box 0 0\n  param shape box\nnode 2 output 2 output 200 0\n", old, error));
    const sim::Compiled before = old.compile();
    CHECK(before.solids.size() == 1 && before.solids[0].matte == sim::Matte::None);
    CHECK(before.look.floorMatte == sim::Matte::Catcher);
    CHECK(before.camera.plate.empty() && before.camera.plateFile(1).empty());

    // A matchmove's camera: the plate's frames are the shot's time codes.
    const std::string usd = dir.write("cam.usda", R"(#usda 1.0
(
    endTimeCode = 1003
    startTimeCode = 1001
    timeCodesPerSecond = 24
)

def Camera "cam"
{
    double3 xformOp:translate.timeSamples = {
        1001: (0, 1, 5),
        1003: (1, 1, 5),
    }
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)");
    sim::Network shot;
    const int ucam = shot.add("usd_camera");
    const int uout = shot.add("output");
    CHECK(shot.connect(ucam, "camera", uout, "camera"));
    CHECK(shot.setText(ucam, "file", usd));
    CHECK(shot.setText(ucam, "plate", "plate.$F4.exr"));
    shot.setParam(uout, "fps", "24");
    shot.setParam(uout, "frames", "3");
    c = shot.compile(dir.path.string());
    CHECK(c.hasCamera);
    CHECK_EQ(c.cameraAt(1).plateFile(1), dir / "plate.1001.exr");
    CHECK_EQ(c.cameraAt(3).plateFile(3), dir / "plate.1003.exr");
    // An offset in the file moves the plate with it; a number given is the plate's own.
    shot.setParam(ucam, "offset", "1");
    CHECK_EQ(shot.compile(dir.path.string()).cameraAt(1).plateFile(1), dir / "plate.1002.exr");
    shot.setParam(ucam, "plate_frame", "7");
    CHECK_EQ(shot.compile(dir.path.string()).cameraAt(2).plateFile(2), dir / "plate.0008.exr");
}
