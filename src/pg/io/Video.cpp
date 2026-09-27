#include "pg/io/Video.h"

#include "pg/io/Jpeg.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <process.h>
#define PG_POPEN _popen
#define PG_PCLOSE _pclose
#else
#include <sys/wait.h>
#include <unistd.h>
#define PG_POPEN popen
#define PG_PCLOSE pclose
#endif

namespace fs = std::filesystem;

namespace pg::io {
namespace {

std::string extensionOf(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// --- Motion JPEG in AVI -----------------------------------------------------------------------------

/// An AVI file (RIFF, AVI 1.0) of one video stream of JPEG frames, with an
/// index: the header is written first with room for the counts, which are
/// filled in once the frames are there.
class AviWriter final : public VideoWriter {
public:
    AviWriter(std::string path, int width, int height, double fps)
        : path_(std::move(path)), width_(width), height_(height), fps_(fps) {}

    bool open(std::string& error) {
        file_.open(path_, std::ios::binary | std::ios::trunc);
        if (!file_) {
            error = path_ + ": cannot write it";
            return false;
        }
        uint32_t rate = 0, scale = 0;
        frameRate(fps_, rate, scale);
        std::string h;
        auto fourcc = [&](const char* c) { h.append(c, 4); };
        auto u32 = [&](uint32_t v) {
            for (int i = 0; i < 4; ++i) h.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        };
        auto u16 = [&](uint16_t v) {
            h.push_back(static_cast<char>(v & 0xFF));
            h.push_back(static_cast<char>(v >> 8));
        };
        const uint32_t w = static_cast<uint32_t>(width_), hh = static_cast<uint32_t>(height_);
        fourcc("RIFF");
        u32(0);  // the file's size, less 8: at the end
        fourcc("AVI ");
        fourcc("LIST");
        u32(192);
        fourcc("hdrl");
        fourcc("avih");
        u32(56);
        u32(static_cast<uint32_t>(std::lround(1e6 / std::max(fps_, 1e-3))));  // microseconds a frame
        u32(0);     // the most bytes a second: at the end
        u32(0);     // padding granularity
        u32(0x10);  // AVIF_HASINDEX
        u32(0);     // frames: at the end
        u32(0);     // initial frames
        u32(1);     // streams
        u32(0);     // suggested buffer size: at the end
        u32(w);
        u32(hh);
        for (int i = 0; i < 4; ++i) u32(0);
        fourcc("LIST");
        u32(116);
        fourcc("strl");
        fourcc("strh");
        u32(56);
        fourcc("vids");
        fourcc("MJPG");
        u32(0);  // flags
        u16(0);  // priority
        u16(0);  // language
        u32(0);  // initial frames
        u32(scale);
        u32(rate);
        u32(0);  // start
        u32(0);  // length in frames: at the end
        u32(0);  // suggested buffer size: at the end
        u32(0xFFFFFFFFu);  // quality: the default
        u32(0);            // sample size: frames vary
        u16(0);
        u16(0);
        u16(static_cast<uint16_t>(width_));
        u16(static_cast<uint16_t>(height_));
        fourcc("strf");
        u32(40);
        u32(40);  // BITMAPINFOHEADER
        u32(w);
        u32(hh);
        u16(1);
        u16(24);
        fourcc("MJPG");
        u32(w * hh * 3);
        for (int i = 0; i < 4; ++i) u32(0);
        fourcc("LIST");
        u32(0);  // the frames' size: at the end
        fourcc("movi");
        file_.write(h.data(), static_cast<std::streamsize>(h.size()));
        position_ = h.size();
        if (!file_) {
            error = path_ + ": cannot write it";
            return false;
        }
        return true;
    }

    bool add(const uint8_t* rgb, std::string& error) override {
        const std::string jpeg = encodeJpeg(rgb, width_, height_, 90);
        if (jpeg.empty()) {
            error = "a frame of " + std::to_string(width_) + " x " + std::to_string(height_) + " is not one JPEG holds";
            return false;
        }
        const uint64_t chunk = 8 + jpeg.size() + (jpeg.size() & 1u);
        // RIFF counts in 32 bits, and most players stop at 2 GB.
        if (position_ + chunk + 16 * (index_.size() + 1) + 8 > 0x7FFFFFFFu) {
            error = path_ + ": an AVI stops at 2 GB -- .mp4 (through ffmpeg) holds more";
            return false;
        }
        index_.push_back({static_cast<uint32_t>(position_ - kMovi), static_cast<uint32_t>(jpeg.size())});
        char head[8] = {'0', '0', 'd', 'c'};
        for (int i = 0; i < 4; ++i) head[4 + i] = static_cast<char>((jpeg.size() >> (8 * i)) & 0xFF);
        file_.write(head, 8);
        file_.write(jpeg.data(), static_cast<std::streamsize>(jpeg.size()));
        if (jpeg.size() & 1u) file_.put('\0');
        position_ += chunk;
        biggest_ = std::max<uint64_t>(biggest_, jpeg.size());
        if (!file_) {
            error = path_ + ": cannot write it";
            return false;
        }
        ++frames_;
        return true;
    }

    bool finish(std::string& error) override {
        const uint64_t moviEnd = position_;
        std::string idx = "idx1";
        auto u32 = [&](std::string& s, uint32_t v) {
            for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        };
        u32(idx, static_cast<uint32_t>(16 * index_.size()));
        for (const Entry& e : index_) {
            idx += "00dc";
            u32(idx, 0x10);  // AVIIF_KEYFRAME: every frame is one
            u32(idx, e.offset);
            u32(idx, e.size);
        }
        file_.write(idx.data(), static_cast<std::streamsize>(idx.size()));
        const uint64_t end = moviEnd + idx.size();
        uint32_t rate = 0, scale = 0;
        frameRate(fps_, rate, scale);
        const uint32_t suggested = static_cast<uint32_t>(biggest_ + 8);
        patch(4, static_cast<uint32_t>(end - 8));
        patch(36, static_cast<uint32_t>(std::min<double>(4.0e9, static_cast<double>(biggest_) * fps_)));
        patch(48, static_cast<uint32_t>(frames_));
        patch(60, suggested);
        patch(140, static_cast<uint32_t>(frames_));
        patch(144, suggested);
        patch(kMovi - 4, static_cast<uint32_t>(moviEnd - kMovi));
        file_.close();
        if (!file_) {
            error = path_ + ": cannot write it";
            return false;
        }
        return true;
    }

    std::string codec() const override { return "Motion JPEG"; }

private:
    struct Entry {
        uint32_t offset, size;
    };
    static constexpr uint64_t kMovi = 220;  // where "movi" is: the index counts from it

    void patch(uint64_t at, uint32_t v) {
        char b[4];
        for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
        file_.seekp(static_cast<std::streamoff>(at));
        file_.write(b, 4);
    }

    std::string path_;
    int width_, height_;
    double fps_;
    std::ofstream file_;
    uint64_t position_ = 0, biggest_ = 0;
    std::vector<Entry> index_;
};

// --- through ffmpeg ---------------------------------------------------------------------------------

std::string ffmpegProgram() {
    const char* p = std::getenv("PG_FFMPEG");
    return p && *p ? p : "ffmpeg";
}

/// `s` as one word of the shell's.
std::string shellWord(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string q = "'";
    for (const char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
#endif
}

#ifdef _WIN32
constexpr const char* kQuiet = " 2>NUL";
#else
constexpr const char* kQuiet = " 2>/dev/null";
#endif

/// The encoders ffmpeg has, asked once; empty if it does not run.
const std::set<std::string>& ffmpegEncoders() {
    static std::once_flag once;
    static std::set<std::string> encoders;
    std::call_once(once, [] {
        FILE* p = PG_POPEN((shellWord(ffmpegProgram()) + " -hide_banner -encoders" + kQuiet).c_str(), "r");
        if (!p) return;
        char line[512];
        bool listed = false;
        while (std::fgets(line, sizeof line, p)) {
            // " V....D libx264   libx264 H.264 / AVC ..." after the legend's "------".
            std::istringstream words(line);
            std::string flags, name;
            if (!(words >> flags)) continue;
            if (flags.rfind("------", 0) == 0) {
                listed = true;
                continue;
            }
            if (listed && flags.size() == 6 && (words >> name)) encoders.insert(name);
        }
        PG_PCLOSE(p);
    });
    return encoders;
}

/// Frames piped into ffmpeg as raw RGB; what it says goes to a log, read
/// back when it fails.
class FfmpegWriter final : public VideoWriter {
public:
    FfmpegWriter(std::string path, int width, int height, double fps, std::string args, std::string codec)
        : path_(std::move(path)), width_(width), height_(height), fps_(fps), args_(std::move(args)), codec_(std::move(codec)) {}

    ~FfmpegWriter() override {
        if (pipe_) PG_PCLOSE(pipe_);
        restoreSignal();
        std::error_code ec;
        if (!log_.empty()) fs::remove(log_, ec);
    }

    bool open(std::string& error) {
        static std::atomic<int> count{0};
#ifdef _WIN32
        const int pid = _getpid();
#else
        const int pid = static_cast<int>(getpid());
#endif
        std::error_code ec;
        log_ = (fs::temp_directory_path(ec) / ("pgshader-ffmpeg-" + std::to_string(pid) + "-" + std::to_string(++count) + ".log")).string();
        uint32_t rate = 0, scale = 0;
        frameRate(fps_, rate, scale);
        const std::string command = shellWord(ffmpegProgram()) + " -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgb24 -video_size " +
                                    std::to_string(width_) + "x" + std::to_string(height_) + " -framerate " + std::to_string(rate) +
                                    "/" + std::to_string(scale) + " -i - " + args_ + " " + shellWord(path_) + " 2>" + shellWord(log_);
#ifdef SIGPIPE
        // An ffmpeg that stops early makes writing to it fail, not kill us.
        previous_ = std::signal(SIGPIPE, SIG_IGN);
        ignoring_ = true;
#endif
#ifdef _WIN32
        pipe_ = PG_POPEN(command.c_str(), "wb");
#else
        pipe_ = PG_POPEN(command.c_str(), "w");
#endif
        if (!pipe_) {
            error = "cannot start " + ffmpegProgram();
            return false;
        }
        return true;
    }

    bool add(const uint8_t* rgb, std::string& error) override {
        const size_t bytes = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3;
        if (!pipe_ || std::fwrite(rgb, 1, bytes, pipe_) != bytes) {
            error = failure("stopped taking frames");
            return false;
        }
        ++frames_;
        return true;
    }

    bool finish(std::string& error) override {
        if (!pipe_) {
            error = failure("did not start");
            return false;
        }
        const int status = PG_PCLOSE(pipe_);
        pipe_ = nullptr;
        restoreSignal();
#ifdef _WIN32
        const bool ok = status == 0;
#else
        const bool ok = status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
        if (!ok) {
            error = failure("failed");
            return false;
        }
        return true;
    }

    std::string codec() const override { return codec_ + " (ffmpeg)"; }

private:
    /// Why: the last thing ffmpeg said, if anything.
    std::string failure(const std::string& what) const {
        std::ifstream in(log_);
        std::string line, last;
        while (std::getline(in, line)) {
            if (!line.empty()) last = line;
        }
        return "ffmpeg " + what + (last.empty() ? std::string() : ": " + last);
    }

    void restoreSignal() {
#ifdef SIGPIPE
        if (ignoring_) std::signal(SIGPIPE, previous_);
        ignoring_ = false;
#endif
    }

    std::string path_;
    int width_, height_;
    double fps_;
    std::string args_, codec_, log_;
    FILE* pipe_ = nullptr;
#ifdef SIGPIPE
    void (*previous_)(int) = SIG_DFL;
    bool ignoring_ = false;
#endif
};

}  // namespace

void frameRate(double fps, uint32_t& rate, uint32_t& scale) {
    if (!(fps > 0.0) || fps > 1000.0) fps = 30.0;
    if (std::fabs(fps - std::round(fps)) < 1e-4) {
        rate = static_cast<uint32_t>(std::lround(fps));
        scale = 1;
        return;
    }
    uint32_t r = static_cast<uint32_t>(std::lround(fps * 1000.0)), s = 1000;
    const uint32_t g = std::gcd(r, s);
    rate = r / g;
    scale = s / g;
}

bool ffmpegAvailable() { return !ffmpegEncoders().empty(); }

bool isVideoPath(const std::string& path) {
    const std::string e = extensionOf(path);
    return e == ".avi" || e == ".mp4" || e == ".mov" || e == ".mkv" || e == ".webm" || e == ".gif";
}

std::vector<std::string> videoExtensions() {
    if (!ffmpegAvailable()) return {".avi"};
    return {".mp4", ".mov", ".mkv", ".webm", ".gif", ".avi"};
}

std::unique_ptr<VideoWriter> openVideo(const std::string& path, int width, int height, double fps, std::string& error) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        error = "a video of " + std::to_string(width) + " x " + std::to_string(height) + " pixels is not one to make";
        return nullptr;
    }
    const std::string e = extensionOf(path);
    if (e == ".avi") {
        auto w = std::make_unique<AviWriter>(path, width, height, fps);
        if (!w->open(error)) return nullptr;
        return w;
    }
    if (!isVideoPath(path)) {
        error = path + ": a video is .avi (Motion JPEG), or with ffmpeg .mp4, .mov, .mkv, .webm, .gif";
        return nullptr;
    }
    const std::set<std::string>& has = ffmpegEncoders();
    if (has.empty()) {
        error = e + " is written by ffmpeg, which is not installed (or PG_FFMPEG does not name it) -- .avi needs nothing";
        return nullptr;
    }
    // H.264 and VP9 in 4:2:0 want an even size: pad an odd one by a pixel.
    const std::string even = " -vf " + shellWord("pad=ceil(iw/2)*2:ceil(ih/2)*2");
    std::string args, codec;
    if (e == ".gif") {
        if (!has.count("gif")) {
            error = "this ffmpeg writes no GIF";
            return nullptr;
        }
        args = "-vf " + shellWord("split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a") + " -loop 0";
        codec = "GIF";
    } else if (e == ".webm") {
        if (has.count("libvpx-vp9")) {
            args = "-c:v libvpx-vp9 -crf 28 -b:v 0 -row-mt 1 -pix_fmt yuv420p" + even;
            codec = "VP9";
        } else if (has.count("libvpx")) {
            args = "-c:v libvpx -crf 8 -b:v 8M -pix_fmt yuv420p" + even;
            codec = "VP8";
        } else {
            error = "this ffmpeg has no VP9 or VP8 encoder (libvpx) for .webm";
            return nullptr;
        }
    } else {
        if (has.count("libx264")) {
            args = "-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p" + even;
            codec = "H.264";
        } else if (has.count("libopenh264")) {
            args = "-c:v libopenh264 -b:v 16M -pix_fmt yuv420p" + even;
            codec = "H.264";
        } else if (has.count("mpeg4")) {
            args = "-c:v mpeg4 -q:v 2 -pix_fmt yuv420p" + even;
            codec = "MPEG-4 part 2";
        } else {
            error = "this ffmpeg has no H.264 or MPEG-4 encoder for " + e;
            return nullptr;
        }
        if (e == ".mp4" || e == ".mov") args += " -movflags +faststart";
    }
    auto w = std::make_unique<FfmpegWriter>(path, width, height, fps, args, codec);
    if (!w->open(error)) return nullptr;
    return w;
}

}  // namespace pg::io
