#include "pg/sim/VdbGas.h"

#include "pg/core/Half.h"
#include "pg/io/Picture.h"
#include "pg/io/Vdb.h"
#include "pg/sim/SparseGrid.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace pg::sim {
namespace {

namespace fs = std::filesystem;

std::vector<std::string> wordsOf(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

/// "12345:1690000000": a file's size and the time it last changed; empty
/// for one that is not there.
std::string stampOf(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return {};
    const auto time = fs::last_write_time(path, ec);
    if (ec) return {};
    // The count is the library's: __int128 in libc++, which to_string does not take.
    return std::to_string(size) + ":" + std::to_string(static_cast<long long>(time.time_since_epoch().count()));
}

/// The file of frame `frame`.
std::string fileOf(const VdbGas& g, int frame) { return io::sequenceFile(g.file, frame + g.offset); }

/// The first of the space-separated `names` that is a scalar grid in
/// `grids`; empty when none is.
std::string firstOf(const std::string& names, const std::vector<io::VdbGridInfo>& grids) {
    for (const std::string& n : wordsOf(names)) {
        for (const io::VdbGridInfo& g : grids) {
            if (g.name == n && g.readable && g.type.rfind("vec", 0) != 0) return n;
        }
    }
    return {};
}

/// The grids the fields are read from, of a file: density, temperature,
/// flame, steam -- each empty when the file has none of its names.
std::array<std::string, 4> gridsOf(const VdbGas& g, const std::vector<io::VdbGridInfo>& grids) {
    return {firstOf(g.density, grids), firstOf(g.temperature, grids), firstOf(g.flame, grids), firstOf(g.steam, grids)};
}

/// Where the gas of the files is, and how fine.
struct Extent {
    bool any = false;
    Vec3 lo, hi;
    float voxel = 0.0f;
    int missing = 0;  // frames with no file
    int first = 0;    // the first frame with one
    void grow(const Vec3& a, const Vec3& b, float v) {
        lo = any ? glm::min(lo, a) : a;
        hi = any ? glm::max(hi, b) : b;
        voxel = any ? std::min(voxel, v) : v;
        any = true;
    }
};

struct Found {
    Extent extent;
    std::string error;
};

/// What the files of frames 1 to `frames` hold of the grids, where: their
/// headers say -- or, when they do not, their values. Kept, for the same
/// files as they were.
Found extentOf(const VdbGas& g, int frames, const std::string& stamp) {
    static std::mutex mu;
    static std::map<std::string, Found> kept;
    std::ostringstream key;
    key << g.file << '\n' << g.offset << '\n' << g.zUp << '\n' << g.density << '\n' << g.temperature << '\n' << g.flame
        << '\n' << g.steam << '\n' << frames << '\n' << stamp;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (auto it = kept.find(key.str()); it != kept.end()) return it->second;
    }
    Found found;
    Extent& e = found.extent;
    const bool sequence = io::isSequence(g.file);
    for (int f = 1; f <= (sequence ? frames : 1); ++f) {
        const std::string path = fileOf(g, f);
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            ++e.missing;
            continue;
        }
        if (e.first == 0) e.first = f;
        std::vector<io::VdbGridInfo> grids;
        std::string error;
        if (!io::vdbGrids(path, grids, error, g.zUp)) {
            found.error = error;
            break;
        }
        for (const std::string& name : gridsOf(g, grids)) {
            if (name.empty()) continue;
            const auto info = std::find_if(grids.begin(), grids.end(), [&](const io::VdbGridInfo& i) { return i.name == name; });
            if (info->bounded) {
                e.grow(info->lo, info->hi, info->voxel);
                continue;
            }
            // No box in its header: its values say where it is.
            io::VdbVolumes read;
            io::VdbReadOptions o;
            o.grids = {name};
            o.zUp = g.zUp;
            if (!io::readVdb(path, read, error, o)) {
                found.error = error;
                break;
            }
            for (const Volume& v : read.volumes) e.grow(v.origin, v.origin + v.size(), info->voxel);
        }
        if (!found.error.empty()) break;
    }
    std::lock_guard<std::mutex> lk(mu);
    if (kept.size() > 64) kept.clear();
    kept[key.str()] = found;
    return found;
}

int roundUp8(int n) { return std::max(8, (n + 7) / 8 * 8); }

}  // namespace

bool vdbGasDomain(VdbGas& g, int frames, Domain& domain, int& factor, std::vector<std::string>& notes, std::string& error) {
    frames = std::max(frames, 1);
    // The files as they are now: what the frames are compared by.
    const bool sequence = io::isSequence(g.file);
    std::string stamp;
    for (int f = 1; f <= (sequence ? frames : 1); ++f) stamp += stampOf(fileOf(g, f)) + ";";
    g.stamp = std::to_string(std::hash<std::string>()(stamp));
    const Found found = extentOf(g, frames, stamp);
    if (!found.error.empty()) {
        error = found.error;
        return false;
    }
    const Extent& e = found.extent;
    if (e.first == 0) {
        error = sequence ? "no files: " + fileOf(g, 1) + " to " + fileOf(g, frames) + " are not there"
                         : "no file: " + g.file + " is not there";
        return false;
    }
    if (!e.any) {
        error = "no grid of the names given (" + g.density + " / " + g.temperature + " / " + g.flame + ") in " +
                fileOf(g, e.first) + (sequence ? " or the files after it" : "");
        return false;
    }
    if (e.missing > 0) {
        notes.push_back(std::to_string(e.missing) + " of the " + std::to_string(frames) +
                        " frames have no file: they have no gas");
    }
    const Vec3 lo = e.lo + g.move, hi = e.hi + g.move;
    if (lo.y < -0.5f * e.voxel) {
        char text[160];
        std::snprintf(text, sizeof text,
                      "the files hold gas below the floor, down to y = %.2f m: the domain stands on it, and what is "
                      "under it is not drawn -- Move lifts it",
                      static_cast<double>(lo.y));
        notes.push_back(text);
    }
    // The domain, round the y axis on the floor, that holds it all: cells
    // as large as the voxels, or as many of them a side as Resolution asks.
    const float x = std::max(std::fabs(lo.x), std::fabs(hi.x)), y = std::max(hi.y, e.voxel), z = std::max(std::fabs(lo.z), std::fabs(hi.z));
    const int most = std::clamp(g.resolution, 16, 1024);
    factor = 1;
    for (;; ++factor) {
        const float v = e.voxel * static_cast<float>(factor);
        domain.voxel = v;
        domain.cells[0] = roundUp8(2 * static_cast<int>(std::ceil(x / v - 1e-3f)));
        domain.cells[1] = roundUp8(static_cast<int>(std::ceil(y / v - 1e-3f)));
        domain.cells[2] = roundUp8(2 * static_cast<int>(std::ceil(z / v - 1e-3f)));
        if (std::max({domain.cells[0], domain.cells[1], domain.cells[2]}) <= most) break;
    }
    // At least 16 cells along its longest side, as every domain has.
    int& longest = *std::max_element(domain.cells, domain.cells + 3);
    longest = std::max(longest, 16);
    if (factor > 1) {
        notes.push_back("the files' voxels averaged down " + std::to_string(factor) + " \xc3\x97 " + std::to_string(factor) +
                        " \xc3\x97 " + std::to_string(factor) + ": the domain that holds them would be more than " +
                        std::to_string(most) + " cells along a side");
    }
    return true;
}

bool vdbGasFrame(const VdbGas& g, const Domain& domain, int frame, Frame& out, std::string& error) {
    out = Frame();
    out.domain = domain;
    auto empty = [&] {
        out.gasTiles = {0};
        out.fields.assign(3 * Tiles::kCells, 0);
        out.steam.clear();
        return true;
    };
    const std::string path = fileOf(g, frame);
    std::error_code ec;
    if (!fs::exists(path, ec)) return empty();
    std::vector<io::VdbGridInfo> grids;
    if (!io::vdbGrids(path, grids, error, g.zUp)) return false;
    const std::array<std::string, 4> names = gridsOf(g, grids);
    // Read as fine as the domain, near enough: as many voxels a side to a
    // cell as fit.
    io::VdbReadOptions o;
    o.zUp = g.zUp;
    float voxel = 0.0f;
    for (const std::string& n : names) {
        if (n.empty()) continue;
        o.grids.push_back(n);
        for (const io::VdbGridInfo& i : grids) {
            if (i.name == n && i.voxel > 0.0f) voxel = voxel > 0.0f ? std::min(voxel, i.voxel) : i.voxel;
        }
    }
    if (o.grids.empty()) return empty();
    if (voxel > 0.0f) o.downsample = std::max(1, static_cast<int>(std::floor(domain.voxel / voxel + 1e-3f)));
    o.maxVoxels = std::max<size_t>(domain.cellCount() * 2, size_t(1) << 20);
    io::VdbVolumes read;
    if (!io::readVdb(path, read, error, o)) return false;

    // The tiles that hold any, as they are made: three halves a cell, and
    // the steam's one when there is a grid for it.
    const bool steamy = !names[3].empty();
    size_t t[3];
    for (int a = 0; a < 3; ++a) t[a] = static_cast<size_t>((domain.cells[a] + Tiles::kSide - 1) / Tiles::kSide);
    std::unordered_map<uint32_t, uint32_t> slots;
    std::vector<uint16_t> fields, steam;
    // The place of cell (i, j, k) in them, its tile made when it is the first.
    auto cellAt = [&](int i, int j, int k) -> size_t {
        const auto tile = static_cast<uint32_t>(static_cast<size_t>(i >> Tiles::kLog) +
                                                t[0] * (static_cast<size_t>(j >> Tiles::kLog) + t[1] * static_cast<size_t>(k >> Tiles::kLog)));
        const auto [it, made] = slots.emplace(tile, static_cast<uint32_t>(slots.size()));
        if (made) {
            fields.resize(fields.size() + 3 * Tiles::kCells, 0);
            if (steamy) steam.resize(steam.size() + Tiles::kCells, 0);
        }
        return static_cast<size_t>(it->second) * Tiles::kCells + SparseGrid::local(i, j, k);
    };
    const Vec3 origin = domain.origin();
    const float scales[4] = {g.densityScale, g.temperatureScale, g.flameScale, 1.0f};
    for (size_t n = 0; n < read.volumes.size(); ++n) {
        const Volume& v = read.volumes[n];
        const auto which = std::find(names.begin(), names.end(), v.name);
        if (which == names.end() || !v.values || v.values->size() < v.count()) continue;
        const int channel = static_cast<int>(which - names.begin());
        const float scale = std::isfinite(scales[channel]) ? scales[channel] : 1.0f;
        auto put = [&](int i, int j, int k, float value) {
            value *= scale;
            if (!(value > 0.0f)) return;  // nothing, or below it -- or not a number
            const size_t at = cellAt(i, j, k);
            if (channel < 3) fields[3 * at + static_cast<size_t>(channel)] = halfFromFloat(value);
            else steam[at] = halfFromFloat(value);
        };
        const Vec3 shifted = v.origin + g.move;
        if (std::fabs(v.voxel - domain.voxel) <= 1e-3f * domain.voxel) {
            // Voxel for voxel: each on the cell its middle is nearest.
            int shift[3];
            for (int a = 0; a < 3; ++a) shift[a] = static_cast<int>(std::lround((shifted[a] - origin[a]) / domain.voxel));
            for (int k = 0; k < v.res[2]; ++k) {
                const int ck = k + shift[2];
                if (ck < 0 || ck >= domain.cells[2]) continue;
                for (int j = 0; j < v.res[1]; ++j) {
                    const int cj = j + shift[1];
                    if (cj < 0 || cj >= domain.cells[1]) continue;
                    for (int i = 0; i < v.res[0]; ++i) {
                        const int ci = i + shift[0];
                        if (ci < 0 || ci >= domain.cells[0]) continue;
                        put(ci, cj, ck, v.at(i, j, k));
                    }
                }
            }
        } else {
            // Read between its voxels at the middle of each cell it covers.
            int from[3], to[3];
            for (int a = 0; a < 3; ++a) {
                from[a] = std::max(0, static_cast<int>(std::floor((shifted[a] - origin[a]) / domain.voxel)));
                to[a] = std::min(domain.cells[a] - 1,
                                 static_cast<int>(std::ceil((shifted[a] + v.size()[a] - origin[a]) / domain.voxel)));
            }
            for (int k = from[2]; k <= to[2]; ++k) {
                for (int j = from[1]; j <= to[1]; ++j) {
                    for (int i = from[0]; i <= to[0]; ++i) {
                        const Vec3 p = origin + (Vec3(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k)) + 0.5f) * domain.voxel;
                        put(i, j, k, v.sample(p - g.move));
                    }
                }
            }
        }
    }
    if (slots.empty()) return empty();
    // In the order of their numbers, as a frame keeps them.
    std::vector<std::pair<uint32_t, uint32_t>> order(slots.begin(), slots.end());
    std::sort(order.begin(), order.end());
    out.gasTiles.reserve(order.size());
    out.fields.resize(3 * Tiles::kCells * order.size());
    if (steamy) out.steam.resize(Tiles::kCells * order.size());
    for (size_t s = 0; s < order.size(); ++s) {
        out.gasTiles.push_back(order[s].first);
        const size_t from = order[s].second;
        std::copy_n(fields.begin() + static_cast<std::ptrdiff_t>(3 * Tiles::kCells * from), 3 * Tiles::kCells,
                    out.fields.begin() + static_cast<std::ptrdiff_t>(3 * Tiles::kCells * s));
        if (steamy) {
            std::copy_n(steam.begin() + static_cast<std::ptrdiff_t>(Tiles::kCells * from), Tiles::kCells,
                        out.steam.begin() + static_cast<std::ptrdiff_t>(Tiles::kCells * s));
        }
    }
    if (std::all_of(out.steam.begin(), out.steam.end(), [](uint16_t h) { return h == 0; })) out.steam.clear();
    return true;
}

}  // namespace pg::sim
