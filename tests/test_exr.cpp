//
// OpenEXR out (src/pg/io/Exr.h): the header as OpenEXR has it -- channels
// sorted, RLE, windows, strings and matrices --, a line a chunk where the
// offsets say, and the values back through a reader of RLE written here
// from the file layout: halves as halves, floats bit for bit, infinity
// too. Runs get smaller; noise is stored as it is.
//
#include "pg/core/Half.h"
#include "pg/io/Exr.h"

#include "test_framework.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace pg;
namespace fs = std::filesystem;

namespace {

uint32_t u32(const std::string& b, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(b[at + static_cast<size_t>(i)])) << (8 * i);
    return v;
}

uint64_t u64(const std::string& b, size_t at) {
    return static_cast<uint64_t>(u32(b, at)) | (static_cast<uint64_t>(u32(b, at + 4)) << 32);
}

std::string cString(const std::string& b, size_t& at) {
    const size_t end = b.find('\0', at);
    std::string s = b.substr(at, end - at);
    at = end + 1;
    return s;
}

/// A file read back as the layout has it: the header's attributes, then
/// each line -- RLE undone -- into its channels' values.
struct Read {
    std::map<std::string, std::string> attributes;  // name -> value bytes
    std::map<std::string, std::string> types;
    std::vector<std::pair<std::string, int>> channels;  // name, 1 half / 2 float
    std::map<std::string, std::vector<float>> values;
    int width = 0, height = 0;
    bool ok = true;
    size_t packedLines = 0;  // lines that came out smaller
    size_t chunkBytes = 0;   // the lines' bytes in the file
};

std::string unRle(const std::string& in, size_t raw) {
    std::string t;
    size_t i = 0;
    while (i < in.size()) {
        const int count = static_cast<signed char>(in[i++]);
        if (count < 0) {
            t.append(in, i, static_cast<size_t>(-count));
            i += static_cast<size_t>(-count);
        } else {
            t.append(static_cast<size_t>(count) + 1, in[i++]);
        }
    }
    if (t.size() != raw) return {};
    for (size_t k = 1; k < t.size(); ++k) t[k] = static_cast<char>(static_cast<unsigned char>(t[k - 1]) + static_cast<unsigned char>(t[k]) - 128);
    std::string out(raw, '\0');
    size_t even = 0, odd = (raw + 1) / 2;
    for (size_t k = 0; k < raw; ++k) out[k] = t[k % 2 == 0 ? even++ : odd++];
    return out;
}

Read readBack(const std::string& b) {
    Read r;
    if (b.size() < 8 || u32(b, 0) != 20000630u) {
        r.ok = false;
        return r;
    }
    size_t at = 8;
    while (at < b.size() && b[at] != '\0') {
        const std::string name = cString(b, at), type = cString(b, at);
        const uint32_t size = u32(b, at);
        at += 4;
        r.attributes[name] = b.substr(at, size);
        r.types[name] = type;
        at += size;
    }
    ++at;  // the header's end
    const std::string& list = r.attributes["channels"];
    for (size_t c = 0; c < list.size() && list[c] != '\0';) {
        const std::string name = cString(list, c);
        r.channels.emplace_back(name, static_cast<int>(u32(list, c)));
        c += 16;
    }
    const std::string& window = r.attributes["dataWindow"];
    r.width = static_cast<int>(u32(window, 8)) + 1;
    r.height = static_cast<int>(u32(window, 12)) + 1;
    size_t lineBytes = 0;
    for (const auto& [name, type] : r.channels) lineBytes += static_cast<size_t>(r.width) * (type == 1 ? 2 : 4);
    for (int y = 0; y < r.height; ++y) {
        const size_t chunk = static_cast<size_t>(u64(b, at + 8 * static_cast<size_t>(y)));
        if (static_cast<int>(u32(b, chunk)) != y) r.ok = false;
        const uint32_t size = u32(b, chunk + 4);
        r.chunkBytes += size;
        std::string line = b.substr(chunk + 8, size);
        if (size < lineBytes) {
            line = unRle(line, lineBytes);
            ++r.packedLines;
        }
        if (line.size() != lineBytes) {
            r.ok = false;
            return r;
        }
        size_t p = 0;
        for (const auto& [name, type] : r.channels) {
            for (int x = 0; x < r.width; ++x) {
                if (type == 1) {
                    const uint16_t h = static_cast<uint16_t>(static_cast<unsigned char>(line[p]) | (static_cast<unsigned char>(line[p + 1]) << 8));
                    r.values[name].push_back(floatFromHalf(h));
                    p += 2;
                } else {
                    const uint32_t bits = u32(line, p);
                    float f;
                    std::memcpy(&f, &bits, sizeof f);
                    r.values[name].push_back(f);
                    p += 4;
                }
            }
        }
    }
    return r;
}

}  // namespace

TEST(exr_is_laid_out_as_openexr_has_it_and_reads_back) {
    io::ExrImage img;
    img.width = 7;
    img.height = 3;
    const size_t n = 21;
    io::ExrChannel z{"Z", false, {}}, r{"R", true, {}}, mask{"mask.pieces", true, {}};
    for (size_t i = 0; i < n; ++i) {
        z.values.push_back(i == 0 ? std::numeric_limits<float>::infinity() : 0.1f * static_cast<float>(i) + 1e-7f);
        r.values.push_back(static_cast<float>(i) * 0.37f);  // up to 7.4: beyond white
        mask.values.push_back(i % 7 > 3 ? 1.0f : 0.0f);
    }
    img.channels = {z, mask, r};  // any order: the file sorts them
    img.strings = {{"comments", "a test"}};
    img.matrices = {{"worldToCamera", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1}}};
    const std::string bytes = io::formatExr(img);
    CHECK_EQ(u32(bytes, 4), 2u);  // version 2, one part, lines
    const Read back = readBack(bytes);
    CHECK(back.ok);
    CHECK_EQ(back.width, 7);
    CHECK_EQ(back.height, 3);
    CHECK_EQ(back.channels.size(), size_t(3));
    if (back.channels.size() == 3) {
        CHECK_EQ(back.channels[0].first, std::string("R"));  // sorted by name
        CHECK_EQ(back.channels[1].first, std::string("Z"));
        CHECK_EQ(back.channels[2].first, std::string("mask.pieces"));
        CHECK_EQ(back.channels[1].second, 2);  // float
    }
    CHECK_EQ(back.types.at("compression"), std::string("compression"));
    CHECK_EQ(static_cast<int>(back.attributes.at("compression")[0]), 1);  // RLE
    CHECK_EQ(back.attributes.at("comments"), std::string("a test"));
    CHECK_EQ(back.types.at("worldToCamera"), std::string("m44f"));
    CHECK_EQ(back.attributes.at("worldToCamera").size(), size_t(64));
    for (const char* required : {"displayWindow", "lineOrder", "pixelAspectRatio", "screenWindowCenter", "screenWindowWidth"}) {
        CHECK(back.attributes.count(required) == 1);
    }
    // The values: floats bit for bit, halves as halves round them.
    for (size_t i = 0; i < n; ++i) {
        CHECK(std::memcmp(&back.values.at("Z")[i], &z.values[i], sizeof(float)) == 0);
        CHECK_EQ(back.values.at("R")[i], floatFromHalf(halfFromFloat(r.values[i])));
        CHECK_EQ(back.values.at("mask.pieces")[i], mask.values[i]);
    }
}

TEST(exr_lines_of_runs_get_smaller_noise_is_stored_as_it_is) {
    io::ExrImage img;
    img.width = 256;
    img.height = 2;
    io::ExrChannel flat{"A", true, std::vector<float>(512, 1.0f)};
    img.channels = {flat};
    Read back = readBack(io::formatExr(img));
    CHECK(back.ok);
    CHECK_EQ(back.packedLines, size_t(2));
    CHECK(back.chunkBytes < 2 * 512 * 2 / 16);  // 2 lines of 256 halves: a fraction of their bytes
    // Noise does not get smaller: stored as it is, and read all the same.
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-1e6f, 1e6f);
    io::ExrChannel noise{"N", false, {}};
    for (int i = 0; i < 512; ++i) noise.values.push_back(u(rng));
    img.channels = {noise};
    back = readBack(io::formatExr(img));
    CHECK(back.ok);
    CHECK_EQ(back.packedLines, size_t(0));
    for (size_t i = 0; i < 512; ++i) CHECK_EQ(back.values.at("N")[i], noise.values[i]);
}

TEST(exr_refuses_what_is_not_a_picture) {
    const fs::path path = fs::temp_directory_path() / "pg_test_refused.exr";
    std::string error;
    io::ExrImage img;
    CHECK(!io::writeExr(img, path.string(), error));
    CHECK(error.find("no picture") != std::string::npos);
    img.width = 2;
    img.height = 2;
    img.channels = {{"R", true, {1.0f, 2.0f, 3.0f}}};
    CHECK(!io::writeExr(img, path.string(), error));
    CHECK(error.find("3 values, not 4") != std::string::npos);
    img.channels = {{"R", true, std::vector<float>(4, 0.0f)}, {"R", true, std::vector<float>(4, 0.0f)}};
    CHECK(!io::writeExr(img, path.string(), error));
    CHECK(error.find("differ") != std::string::npos);
    img.channels.pop_back();
    CHECK(io::writeExr(img, path.string(), error));
    std::error_code ec;
    fs::remove(path, ec);
}
