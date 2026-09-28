#include "pg/io/Picture.h"

#include "pg/io/Exr.h"
#include "pg/io/Jpeg.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace pg::io {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// An EXR's channels as RGBA: R, G, B and A -- or those of the first layer
/// that has them (beauty.R...) -- else Y as grey, else the first channel.
bool fromExr(const ExrImage& image, Picture& out, std::string& error) {
    const auto find = [&](const std::string& layer, const char* name) -> const ExrChannel* {
        for (const ExrChannel& c : image.channels) {
            const size_t dot = c.name.rfind('.');
            const std::string own = dot == std::string::npos ? c.name : c.name.substr(dot + 1);
            const std::string in = dot == std::string::npos ? std::string() : c.name.substr(0, dot);
            if (in == layer && lower(own) == name) return &c;
        }
        return nullptr;
    };
    std::vector<std::string> layers = {std::string()};
    for (const ExrChannel& c : image.channels) {
        const size_t dot = c.name.rfind('.');
        if (dot != std::string::npos && std::find(layers.begin(), layers.end(), c.name.substr(0, dot)) == layers.end()) {
            layers.push_back(c.name.substr(0, dot));
        }
    }
    const ExrChannel* rgb[3] = {nullptr, nullptr, nullptr};
    const ExrChannel* alpha = nullptr;
    for (const std::string& layer : layers) {
        const ExrChannel *r = find(layer, "r"), *g = find(layer, "g"), *b = find(layer, "b");
        if (r && g && b) {
            rgb[0] = r;
            rgb[1] = g;
            rgb[2] = b;
            alpha = find(layer, "a");
            break;
        }
    }
    if (!rgb[0]) {
        const ExrChannel* grey = find("", "y");
        if (!grey && !image.channels.empty()) grey = &image.channels.front();
        if (!grey) {
            error = "an OpenEXR file without channels";
            return false;
        }
        rgb[0] = rgb[1] = rgb[2] = grey;
        alpha = find("", "a");
    }
    out.width = image.width;
    out.height = image.height;
    out.linear = true;
    const size_t n = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    out.rgba.assign(n * 4, 1.0f);
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) out.rgba[i * 4 + static_cast<size_t>(c)] = rgb[c]->values[i];
        if (alpha) out.rgba[i * 4 + 3] = alpha->values[i];
    }
    return true;
}

std::string padded(int frame, int digits) {
    char text[40];
    std::snprintf(text, sizeof text, "%0*d", std::clamp(digits, 0, 16), frame);
    return text;
}

}  // namespace

bool decodePicture(std::span<const uint8_t> bytes, Picture& out, std::string& error) {
    static constexpr uint8_t kPng[4] = {137, 80, 78, 71};
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), kPng, 4) == 0) {
        Picture p;
        if (!decodePng(bytes, p, error)) return false;
        p.linear = false;
        out = std::move(p);
        return true;
    }
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
        Picture p;
        if (!decodeJpeg(bytes, p, error)) return false;
        p.linear = false;
        out = std::move(p);
        return true;
    }
    if (bytes.size() >= 4 && bytes[0] == 0x76 && bytes[1] == 0x2F && bytes[2] == 0x31 && bytes[3] == 0x01) {
        ExrImage image;
        if (!parseExr(bytes, image, error)) return false;
        Picture p;
        if (!fromExr(image, p, error)) return false;
        out = std::move(p);
        return true;
    }
    error = "not a picture this reads (PNG, JPEG, OpenEXR)";
    return false;
}

bool readPicture(const std::string& path, Picture& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!decodePicture(bytes, out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool writePicture(const std::string& path, const Picture& picture, int quality, std::string& error) {
    const size_t n = picture.empty() ? 0 : static_cast<size_t>(picture.width) * static_cast<size_t>(picture.height);
    if (n == 0 || picture.rgba.size() < n * 4) {
        error = path + ": no picture to write";
        return false;
    }
    bool opaque = true;
    for (size_t i = 0; i < n && opaque; ++i) opaque = picture.rgba[i * 4 + 3] == 1.0f;
    const std::string ext = lower(std::filesystem::path(path).extension().string());
    if (ext == ".exr") {
        ExrImage image;
        image.width = picture.width;
        image.height = picture.height;
        const char* names[4] = {"R", "G", "B", "A"};
        for (size_t c = 0; c < (opaque ? 3u : 4u); ++c) {
            ExrChannel channel{names[c], true, std::vector<float>(n)};
            for (size_t i = 0; i < n; ++i) channel.values[i] = picture.rgba[i * 4 + c];
            image.channels.push_back(std::move(channel));
        }
        return writeExr(image, path, error);
    }
    // As shown: 0 to 1, rounded to 8 bits.
    const auto byte = [](float v) {
        return static_cast<uint8_t>(std::lround(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f) * 255.0f));
    };
    std::string bytes;
    if (ext == ".png") {
        const size_t channels = opaque ? 3 : 4;
        std::vector<uint8_t> pixels(n * channels);
        for (size_t i = 0; i < n; ++i) {
            for (size_t c = 0; c < channels; ++c) pixels[i * channels + c] = byte(picture.rgba[i * 4 + c]);
        }
        bytes = encodePng(pixels.data(), picture.width, picture.height, static_cast<int>(channels));
    } else if (ext == ".jpg" || ext == ".jpeg") {
        std::vector<uint8_t> pixels(n * 3);
        for (size_t i = 0; i < n; ++i) {
            for (size_t c = 0; c < 3; ++c) pixels[i * 3 + c] = byte(picture.rgba[i * 4 + c]);
        }
        bytes = encodeJpeg(pixels.data(), picture.width, picture.height, std::clamp(quality, 1, 100));
    } else {
        error = path + ": a picture is written as .png, .jpg or .exr";
        return false;
    }
    if (bytes.empty()) {
        error = path + ": a picture of " + std::to_string(picture.width) + " x " + std::to_string(picture.height) +
                " pixels is not written as " + ext;
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

float srgbToLinear(float v) {
    if (v <= 0.04045f) return v / 12.92f;
    return std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float v) {
    if (v <= 0.0031308f) return v * 12.92f;
    return 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

std::string sequenceFile(const std::string& pattern, int frame) {
    std::string out;
    for (size_t i = 0; i < pattern.size();) {
        const char c = pattern[i];
        if (c == '#') {
            size_t j = i;
            while (j < pattern.size() && pattern[j] == '#') ++j;
            out += padded(frame, static_cast<int>(j - i));
            i = j;
            continue;
        }
        if (c == '$' && i + 1 < pattern.size() && pattern[i + 1] == 'F') {
            size_t j = i + 2;
            int digits = 0;
            while (j < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[j])) && digits < 100) {
                digits = digits * 10 + (pattern[j] - '0');
                ++j;
            }
            out += padded(frame, digits);
            i = j;
            continue;
        }
        if (c == '%') {
            size_t j = i + 1;
            int digits = 0;
            while (j < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[j])) && digits < 100) {
                digits = digits * 10 + (pattern[j] - '0');
                ++j;
            }
            if (j < pattern.size() && pattern[j] == 'd') {
                out += padded(frame, digits);
                i = j + 1;
                continue;
            }
        }
        out += c;
        ++i;
    }
    return out;
}

bool isSequence(const std::string& pattern) { return sequenceFile(pattern, 1) != sequenceFile(pattern, 2); }

}  // namespace pg::io
