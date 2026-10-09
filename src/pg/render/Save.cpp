#include "pg/render/Save.h"

#include "pg/io/Exr.h"
#include "pg/io/Picture.h"
#include "pg/render/Plate.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
    r.ocio = tracer.settings().ocio;
    r.space = tracer.settings().exrSpace;
    r.transparent = tracer.scene()->look.transparent;
    r.grain = tracer.settings().grain;
    r.grainSeed = static_cast<uint32_t>(std::lround(tracer.scene()->time * 1000.0f));
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
    std::vector<uint8_t> rgba = toDisplay(composited(rendered), rendered.exposure, rendered.view, rendered.ocio.get());
    addGrain(rgba, rendered.beauty.width, rendered.beauty.height, rendered.grain, rendered.grainSeed);
    std::vector<uint8_t> rgb(rgba.size() / 4 * 3);
    for (size_t p = 0; p < rgba.size() / 4; ++p) {
        rgb[3 * p] = rgba[4 * p];
        rgb[3 * p + 1] = rgba[4 * p + 1];
        rgb[3 * p + 2] = rgba[4 * p + 2];
    }
    return rgb;
}

Image transparentAlpha(const Rendered& rendered) {
    const Image& cg = rendered.beauty;
    Image out;
    out.width = cg.width;
    out.height = cg.height;
    out.channels = 1;
    const size_t n = static_cast<size_t>(std::max(cg.width, 0)) * static_cast<size_t>(std::max(cg.height, 0));
    out.pixels.assign(n, 1.0f);
    const bool alpha = rendered.alpha.width == cg.width && rendered.alpha.height == cg.height &&
                       rendered.alpha.pixels.size() >= n * static_cast<size_t>(rendered.alpha.channels);
    const bool catcher = rendered.catcher.width == cg.width && rendered.catcher.height == cg.height &&
                         rendered.catcher.channels >= 3 && rendered.catcher.pixels.size() >= 3 * n;
    if (!alpha) return out;
    const size_t ka = static_cast<size_t>(rendered.alpha.channels), kc = static_cast<size_t>(rendered.catcher.channels);
    for (size_t p = 0; p < n; ++p) {
        const float a = std::clamp(rendered.alpha.pixels[p * ka], 0.0f, 1.0f);
        float shadow = 0.0f;
        if (catcher) {
            const float* c = &rendered.catcher.pixels[p * kc];
            shadow = std::clamp(1.0f - (c[0] + c[1] + c[2]) / 3.0f, 0.0f, 1.0f);
        }
        out.pixels[p] = a + (1.0f - a) * shadow;
    }
    return out;
}

std::vector<uint8_t> displayRgba(const Rendered& rendered) {
    if (!rendered.transparent) {
        std::vector<uint8_t> rgba = toDisplay(composited(rendered), rendered.exposure, rendered.view, rendered.ocio.get());
        addGrain(rgba, rendered.beauty.width, rendered.beauty.height, rendered.grain, rendered.grainSeed);
        for (size_t p = 3; p < rgba.size(); p += 4) rgba[p] = 255;
        return rgba;
    }
    // The CG's light, premultiplied, divided by how much it covers: the
    // colour a picture with alpha keeps -- shown as the rest is.
    const Image alpha = transparentAlpha(rendered);
    Image straight = rendered.beauty;
    const size_t k = static_cast<size_t>(straight.channels);
    for (size_t p = 0; p < alpha.pixels.size(); ++p) {
        const float a = alpha.pixels[p];
        for (size_t c = 0; c < std::min<size_t>(k, 3); ++c) {
            float& v = straight.pixels[p * k + c];
            v = a > 1e-4f ? v / a : 0.0f;
        }
    }
    std::vector<uint8_t> rgba = toDisplay(straight, rendered.exposure, rendered.view, rendered.ocio.get());
    addGrain(rgba, rendered.beauty.width, rendered.beauty.height, rendered.grain, rendered.grainSeed);
    for (size_t p = 0; p < alpha.pixels.size() && 4 * p + 3 < rgba.size(); ++p) {
        rgba[4 * p + 3] = static_cast<uint8_t>(std::lround(std::clamp(alpha.pixels[p], 0.0f, 1.0f) * 255.0f));
    }
    return rgba;
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
        // Transparent: with its alpha; else as a screen shows it.
        const std::vector<uint8_t> pixels = rendered.transparent ? displayRgba(rendered) : displayRgb(rendered);
        const std::string png = io::encodePng(pixels.data(), image.width, image.height, rendered.transparent ? 4 : 3);
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
    if (rendered.transparent) {
        channel("A", transparentAlpha(rendered), 0, true);  // the shadows too: there is no plate to darken
    } else if (over && rendered.alpha.width == image.width && rendered.alpha.height == image.height) {
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
