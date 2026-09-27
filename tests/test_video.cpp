//
// Pictures and videos out: baseline JPEG (src/pg/io/Jpeg.h), Motion JPEG in
// AVI and the videos ffmpeg writes (Video.h). Where ffmpeg is installed, what
// is written is also decoded by it and compared with what went in.
//
#include "pg/io/Jpeg.h"
#include "pg/io/Video.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace pg;
namespace fs = std::filesystem;

namespace {

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
};

std::string fileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// A picture with smooth gradients, a moving bar and hard edges.
std::vector<uint8_t> picture(int w, int h, int frame = 0) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &rgb[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3];
            p[0] = static_cast<uint8_t>(40 + 180 * x / std::max(w - 1, 1));
            p[1] = static_cast<uint8_t>(40 + 180 * y / std::max(h - 1, 1));
            p[2] = static_cast<uint8_t>(std::abs(x - (frame * 7) % std::max(w, 1)) < 6 ? 230 : 60);
        }
    }
    return rgb;
}

uint32_t u32(const std::string& s, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(s[at + static_cast<size_t>(i)])) << (8 * i);
    return v;
}

int u16be(const std::string& s, size_t at) {
    return (static_cast<unsigned char>(s[at]) << 8) | static_cast<unsigned char>(s[at + 1]);
}

/// The markers of a JPEG in order, and where each is; the scan's data is
/// skipped (0xFF 0x00 is a byte of it, not a marker).
std::vector<std::pair<int, size_t>> markers(const std::string& j) {
    std::vector<std::pair<int, size_t>> out;
    size_t i = 0;
    while (i + 1 < j.size()) {
        if (static_cast<unsigned char>(j[i]) != 0xFF) return {};
        const int m = static_cast<unsigned char>(j[i + 1]);
        out.emplace_back(m, i);
        if (m == 0xD8 || m == 0xD9) {
            i += 2;
            continue;
        }
        const size_t length = static_cast<size_t>(u16be(j, i + 2));
        i += 2 + length;
        if (m == 0xDA) {
            // The entropy-coded data, up to the next marker that is not stuffing.
            while (i + 1 < j.size() && !(static_cast<unsigned char>(j[i]) == 0xFF && j[i + 1] != '\0')) ++i;
        }
    }
    return out;
}

/// Mean squared error to PSNR, dB.
double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double mse = 0.0;
    for (size_t i = 0; i < a.size(); ++i) mse += (a[i] - b[i]) * static_cast<double>(a[i] - b[i]);
    mse /= static_cast<double>(a.size());
    return 10.0 * std::log10(255.0 * 255.0 / std::max(mse, 1e-9));
}

/// Frame `n` of a video as RGB, decoded by ffmpeg.
std::vector<uint8_t> decoded(const std::string& video, int n, size_t bytes, const std::string& raw) {
    const std::string command = "ffmpeg -v error -y -i '" + video + "' -vf 'select=eq(n\\," + std::to_string(n) +
                                ")' -vframes 1 -f rawvideo -pix_fmt rgb24 '" + raw + "' 2>/dev/null";
    if (std::system(command.c_str()) != 0) return {};
    const std::string data = fileText(raw);
    if (data.size() != bytes) return {};
    return std::vector<uint8_t>(data.begin(), data.end());
}

}  // namespace

TEST(jpeg_is_baseline_with_every_segment_in_its_place) {
    const auto rgb = picture(37, 21);
    const std::string j = io::encodeJpeg(rgb.data(), 37, 21, 90);
    const auto m = markers(j);
    // SOI, APP0 (JFIF), DQT, SOF0, DHT, SOS, EOI.
    const int order[] = {0xD8, 0xE0, 0xDB, 0xC0, 0xC4, 0xDA, 0xD9};
    CHECK_EQ(m.size(), std::size(order));
    for (size_t i = 0; i < m.size() && i < std::size(order); ++i) CHECK_EQ(m[i].first, order[i]);
    CHECK(j.compare(m[1].second + 4, 5, std::string("JFIF\0", 5)) == 0);
    // The frame: 8 bits, 21 rows, 37 columns, Y at 2 x 2 and the colour at 1 x 1.
    const size_t sof = m[3].second;
    CHECK_EQ(static_cast<int>(static_cast<unsigned char>(j[sof + 4])), 8);
    CHECK_EQ(u16be(j, sof + 5), 21);
    CHECK_EQ(u16be(j, sof + 7), 37);
    CHECK_EQ(static_cast<int>(static_cast<unsigned char>(j[sof + 11])), 0x22);
    CHECK_EQ(static_cast<int>(static_cast<unsigned char>(j[sof + 14])), 0x11);
    // The same picture, the same bytes; better quality, more of them.
    CHECK(io::encodeJpeg(rgb.data(), 37, 21, 90) == j);
    CHECK(io::encodeJpeg(rgb.data(), 37, 21, 98).size() > io::encodeJpeg(rgb.data(), 37, 21, 50).size());
    // No picture, no JPEG.
    CHECK(io::encodeJpeg(rgb.data(), 0, 21, 90).empty());
    CHECK(io::encodeJpeg(rgb.data(), 70000, 1, 90).empty());
    CHECK(io::encodeJpeg(nullptr, 4, 4, 90).empty());
}

TEST(frame_rates_are_fractions) {
    uint32_t rate = 0, scale = 0;
    io::frameRate(30.0, rate, scale);
    CHECK(rate == 30 && scale == 1);
    io::frameRate(29.97, rate, scale);
    CHECK(rate == 2997 && scale == 100);
    io::frameRate(23.976, rate, scale);
    CHECK(rate == 2997 && scale == 125);
    io::frameRate(12.5, rate, scale);
    CHECK(rate == 25 && scale == 2);
    io::frameRate(0.0, rate, scale);  // none: 30
    CHECK(rate == 30 && scale == 1);
}

TEST(an_avi_is_riff_with_a_jpeg_a_frame_and_an_index) {
    TempFolder dir("avi");
    const int w = 45, h = 33, n = 7;
    std::string error;
    auto video = io::openVideo(dir / "shot.avi", w, h, 24.0, error);
    CHECK(video != nullptr);
    CHECK_EQ(video->codec(), std::string("Motion JPEG"));
    for (int f = 0; f < n; ++f) CHECK(video->add(picture(w, h, f).data(), error));
    CHECK_EQ(video->frames(), n);
    CHECK(video->finish(error));

    const std::string a = fileText(dir / "shot.avi");
    CHECK(a.compare(0, 4, "RIFF") == 0 && a.compare(8, 4, "AVI ") == 0);
    CHECK_EQ(u32(a, 4), static_cast<uint32_t>(a.size() - 8));
    // The main header: a frame's microseconds, the frames, the size.
    CHECK(a.compare(24, 4, "avih") == 0);
    CHECK_EQ(u32(a, 32), 41667u);
    CHECK_EQ(u32(a, 48), static_cast<uint32_t>(n));
    CHECK(u32(a, 64) == static_cast<uint32_t>(w) && u32(a, 68) == static_cast<uint32_t>(h));
    // The stream: video, Motion JPEG, 24 frames a second.
    CHECK(a.compare(108, 4, "vids") == 0 && a.compare(112, 4, "MJPG") == 0);
    CHECK(u32(a, 132) == 24 && u32(a, 128) == 1);
    CHECK_EQ(u32(a, 140), static_cast<uint32_t>(n));
    CHECK(a.compare(188, 4, "MJPG") == 0);
    // The frames, and an index that finds each: a whole JPEG.
    const size_t movi = a.find("movi");
    CHECK_EQ(movi, size_t(220));
    const size_t idx = a.find("idx1", movi);
    CHECK(idx != std::string::npos);
    CHECK_EQ(u32(a, idx + 4), static_cast<uint32_t>(16 * n));
    CHECK_EQ(u32(a, 216), static_cast<uint32_t>(idx - movi));
    for (int f = 0; f < n; ++f) {
        const size_t e = idx + 8 + 16 * static_cast<size_t>(f);
        CHECK(a.compare(e, 4, "00dc") == 0);
        CHECK_EQ(u32(a, e + 4), 0x10u);  // a key frame
        const size_t chunk = movi + u32(a, e + 8), size = u32(a, e + 12);
        CHECK(a.compare(chunk, 4, "00dc") == 0);
        CHECK_EQ(u32(a, chunk + 4), static_cast<uint32_t>(size));
        CHECK(static_cast<unsigned char>(a[chunk + 8]) == 0xFF && static_cast<unsigned char>(a[chunk + 9]) == 0xD8);
        CHECK(static_cast<unsigned char>(a[chunk + 8 + size - 2]) == 0xFF && static_cast<unsigned char>(a[chunk + 8 + size - 1]) == 0xD9);
        CHECK(a.substr(chunk + 8, size) == io::encodeJpeg(picture(w, h, f).data(), w, h, 90));
    }
}

TEST(videos_say_what_they_cannot_be) {
    TempFolder dir("video_errors");
    std::string error;
    CHECK(io::openVideo(dir / "a.txt", 16, 16, 30.0, error) == nullptr);
    CHECK(error.find(".avi") != std::string::npos);
    CHECK(io::openVideo(dir / "a.avi", 0, 16, 30.0, error) == nullptr);
    CHECK(io::openVideo(dir / "no/such/folder/a.avi", 16, 16, 30.0, error) == nullptr);
    CHECK(error.find("cannot write") != std::string::npos);
    CHECK(io::isVideoPath("x/y/Shot.MP4") && io::isVideoPath("a.avi") && io::isVideoPath("a.gif"));
    CHECK(!io::isVideoPath("a.png") && !io::isVideoPath("avi"));
    const auto kinds = io::videoExtensions();
    CHECK(std::find(kinds.begin(), kinds.end(), ".avi") != kinds.end());
    if (!io::ffmpegAvailable()) {
        CHECK_EQ(kinds.size(), size_t(1));
        CHECK(io::openVideo(dir / "a.mp4", 16, 16, 30.0, error) == nullptr);
        CHECK(error.find("ffmpeg") != std::string::npos);
    } else {
        CHECK_EQ(kinds.front(), std::string(".mp4"));
    }
}

TEST(videos_decode_to_what_went_in_where_ffmpeg_is) {
    if (!io::ffmpegAvailable()) return;  // nothing here to decode them with
    TempFolder dir("video_decode");
    const int w = 96, h = 64;
    const size_t bytes = static_cast<size_t>(w) * h * 3;
    for (const char* name : {"clip.avi", "clip.mp4", "clip.webm"}) {
        std::string error;
        auto video = io::openVideo(dir / name, w, h, 30.0, error);
        CHECK(video != nullptr);
        if (!video) continue;
        for (int f = 0; f < 12; ++f) CHECK(video->add(picture(w, h, f).data(), error));
        CHECK(video->finish(error));
        // Frame 5 comes back as frame 5 -- in its place, and close.
        const auto back = decoded(dir / name, 5, bytes, dir / "frame.rgb");
        CHECK_EQ(back.size(), bytes);
        if (back.size() != bytes) continue;
        const double near = psnr(back, picture(w, h, 5)), other = psnr(back, picture(w, h, 9));
        if (!(near > 30.0 && near > other + 3.0)) {
            ::testing::fail(__FILE__, __LINE__, std::string(name) + ": frame 5 at " + std::to_string(near) + " dB, frame 9 at " +
                                                    std::to_string(other) + " dB");
        }
    }
    // An odd size is padded to an even one for H.264; the AVI keeps it.
    std::string error;
    auto odd = io::openVideo(dir / "odd.mp4", 33, 17, 30.0, error);
    CHECK(odd != nullptr);
    if (odd) {
        CHECK(odd->add(picture(33, 17).data(), error));
        CHECK(odd->finish(error));
        CHECK(fs::file_size(dir / "odd.mp4") > 0);
    }
}
