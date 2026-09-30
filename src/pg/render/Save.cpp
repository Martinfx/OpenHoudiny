#include "pg/render/Save.h"

#include "pg/io/Exr.h"
#include "pg/io/Picture.h"

#include <cctype>
#include <filesystem>
#include <fstream>

namespace pg::render {

std::vector<uint8_t> displayRgb(const PathTracer& tracer, bool denoise) {
    const std::vector<uint8_t> rgba = tracer.display(denoise);
    std::vector<uint8_t> rgb(rgba.size() / 4 * 3);
    for (size_t p = 0; p < rgba.size() / 4; ++p) {
        rgb[3 * p] = rgba[4 * p];
        rgb[3 * p + 1] = rgba[4 * p + 1];
        rgb[3 * p + 2] = rgba[4 * p + 2];
    }
    return rgb;
}

bool savePicture(const PathTracer& tracer, const std::string& path, bool denoise, const std::string& comment,
                 std::string& error) {
    if (!tracer.scene() || tracer.samples() == 0) {
        error = "nothing rendered yet";
        return false;
    }
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".exr") {
        const Settings& s = tracer.settings();
        const std::vector<uint8_t> rgb = displayRgb(tracer, denoise);
        const std::string png = io::encodePng(rgb.data(), s.width, s.height, 3);
        std::ofstream file(path, std::ios::binary);
        if (!file || !file.write(png.data(), static_cast<std::streamsize>(png.size()))) {
            error = "cannot write " + path;
            return false;
        }
        return true;
    }
    const Image image = denoise ? tracer.denoised() : tracer.beauty();
    io::ExrImage out;
    out.width = image.width;
    out.height = image.height;
    const size_t n = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    auto channel = [&](const char* name, const Image& from, int c, bool half) {
        io::ExrChannel ch;
        ch.name = name;
        ch.half = half;
        ch.values.resize(n);
        for (size_t p = 0; p < n; ++p) ch.values[p] = from.pixels[p * static_cast<size_t>(from.channels) + static_cast<size_t>(c)];
        out.channels.push_back(std::move(ch));
    };
    channel("R", image, 0, true);
    channel("G", image, 1, true);
    channel("B", image, 2, true);
    io::ExrChannel alpha;
    alpha.name = "A";
    alpha.values.assign(n, 1.0f);
    out.channels.push_back(std::move(alpha));
    channel("Z", tracer.depth(), 0, false);
    const Image albedo = tracer.albedo(), normal = tracer.normal();
    channel("albedo.R", albedo, 0, true);
    channel("albedo.G", albedo, 1, true);
    channel("albedo.B", albedo, 2, true);
    channel("N.X", normal, 0, true);
    channel("N.Y", normal, 1, true);
    channel("N.Z", normal, 2, true);
    if (!comment.empty()) out.strings.push_back({"comment", comment});
    return io::writeExr(out, path, error);
}

}  // namespace pg::render
