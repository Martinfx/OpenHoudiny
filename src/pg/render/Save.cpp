#include "pg/render/Save.h"

#include "pg/io/Exr.h"
#include "pg/io/Picture.h"
#include "pg/render/Plate.h"

#include <cctype>
#include <filesystem>
#include <fstream>

namespace pg::render {

Rendered renderedOf(const PathTracer& tracer, bool denoise) {
    Rendered r;
    if (!tracer.scene() || tracer.samples() == 0) return r;
    r.beauty = denoise ? tracer.denoised() : tracer.beauty();
    r.albedo = tracer.albedo();
    r.normal = tracer.normal();
    r.depth = tracer.depth();
    r.exposure = tracer.scene()->look.exposure;
    r.view = tracer.settings().view;
    r.space = tracer.settings().exrSpace;
    if (const auto& plate = tracer.scene()->plate) {
        r.alpha = tracer.alpha();
        r.catcher = tracer.catcher(denoise);
        r.plate = plateSeen(*plate, tracer.plateLight(), tracer.scene()->camera, r.beauty.width, r.beauty.height);
    }
    return r;
}

Image composited(const Rendered& rendered) {
    if (rendered.plate.pixels.empty()) return rendered.beauty;
    return overPlate(rendered.beauty, rendered.alpha, rendered.catcher, rendered.plate);
}

std::vector<uint8_t> displayRgb(const Rendered& rendered) {
    const std::vector<uint8_t> rgba = toDisplay(composited(rendered), rendered.exposure, rendered.view);
    std::vector<uint8_t> rgb(rgba.size() / 4 * 3);
    for (size_t p = 0; p < rgba.size() / 4; ++p) {
        rgb[3 * p] = rgba[4 * p];
        rgb[3 * p + 1] = rgba[4 * p + 1];
        rgb[3 * p + 2] = rgba[4 * p + 2];
    }
    return rgb;
}

std::vector<uint8_t> displayRgb(const PathTracer& tracer, bool denoise) {
    return displayRgb(renderedOf(tracer, denoise));
}

bool savePicture(const Rendered& rendered, const std::string& path, const std::string& comment, std::string& error) {
    const Image& image = rendered.beauty;
    const size_t n = static_cast<size_t>(std::max(image.width, 0)) * static_cast<size_t>(std::max(image.height, 0));
    if (n == 0 || image.pixels.size() < n * static_cast<size_t>(image.channels)) {
        error = "nothing rendered yet";
        return false;
    }
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".exr") {
        const std::vector<uint8_t> rgb = displayRgb(rendered);
        const std::string png = io::encodePng(rgb.data(), image.width, image.height, 3);
        std::ofstream file(path, std::ios::binary);
        if (!file || !file.write(png.data(), static_cast<std::streamsize>(png.size()))) {
            error = "cannot write " + path;
            return false;
        }
        return true;
    }
    io::ExrImage out;
    out.width = image.width;
    out.height = image.height;
    out.hasChromaticities = true;
    out.chromaticities = chromaticitiesOf(rendered.space);
    // The light and the colours in the space asked for.
    auto inSpace = [&](const Image& from) {
        if (rendered.space == LinearSpace::Rec709 || from.channels < 3) return from;
        Image to = from;
        const size_t k = static_cast<size_t>(from.channels);
        for (size_t p = 0; p + 1 <= to.pixels.size() / k; ++p) {
            float* v = &to.pixels[p * k];
            const Vec3 c = fromRec709(Vec3(v[0], v[1], v[2]), rendered.space);
            v[0] = c.x;
            v[1] = c.y;
            v[2] = c.z;
        }
        return to;
    };
    const Image light = inSpace(image), albedo = inSpace(rendered.albedo);
    // A pass the renderer did not make, or of another size: left out.
    auto channel = [&](const char* name, const Image& from, int c, bool half) {
        if (from.width != image.width || from.height != image.height || c >= from.channels ||
            from.pixels.size() < n * static_cast<size_t>(from.channels)) {
            return;
        }
        io::ExrChannel ch;
        ch.name = name;
        ch.half = half;
        ch.values.resize(n);
        for (size_t p = 0; p < n; ++p) ch.values[p] = from.pixels[p * static_cast<size_t>(from.channels) + static_cast<size_t>(c)];
        out.channels.push_back(std::move(ch));
    };
    channel("R", light, 0, true);
    channel("G", light, std::min(1, light.channels - 1), true);
    channel("B", light, std::min(2, light.channels - 1), true);
    // Over a plate, the CG alone: how much of each pixel it covers, and
    // what the plate is multiplied by there.
    const bool over = !rendered.plate.pixels.empty();
    if (over && rendered.alpha.width == image.width && rendered.alpha.height == image.height) {
        channel("A", rendered.alpha, 0, true);
    } else {
        io::ExrChannel alpha;
        alpha.name = "A";
        alpha.values.assign(n, 1.0f);
        out.channels.push_back(std::move(alpha));
    }
    channel("Z", rendered.depth, 0, false);
    channel("albedo.R", albedo, 0, true);
    channel("albedo.G", albedo, 1, true);
    channel("albedo.B", albedo, 2, true);
    channel("N.X", rendered.normal, 0, true);
    channel("N.Y", rendered.normal, 1, true);
    channel("N.Z", rendered.normal, 2, true);
    if (over) {
        channel("catcher.R", rendered.catcher, 0, true);
        channel("catcher.G", rendered.catcher, 1, true);
        channel("catcher.B", rendered.catcher, 2, true);
    }
    if (!comment.empty()) out.strings.push_back({"comment", comment});
    return io::writeExr(out, path, error);
}

bool savePicture(const PathTracer& tracer, const std::string& path, bool denoise, const std::string& comment,
                 std::string& error) {
    if (!tracer.scene() || tracer.samples() == 0) {
        error = "nothing rendered yet";
        return false;
    }
    return savePicture(renderedOf(tracer, denoise), path, comment, error);
}

}  // namespace pg::render
