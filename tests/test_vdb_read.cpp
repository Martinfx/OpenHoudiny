//
// OpenVDB files read: Blosc frames, and grids written by the library itself
// -- OpenVDB 10 through pyopenvdb (make_vdb.py), OpenVDB 13 (make_vdb13.cpp)
// -- checked voxel by voxel against the rules they were made by.
//
#include "pg/core/Half.h"
#include "pg/io/Blosc.h"
#include "pg/io/Vdb.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/VdbGas.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace pg;
namespace fs = std::filesystem;

namespace {

/// A folder of its own under the system's temporary one, gone afterwards.
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

/// A ball of smoke 0.6 m across at (0.1 f, 0.5, -0.2) for frame f -- its
/// density 1 in the middle, 0 at its edge -- twice as hot; burning from
/// frame 3: the frames of a little shot, as OpenVDB files.
std::vector<Volume> puff(int f) {
    const float voxel = 0.05f;
    const Vec3 middle(0.1f * static_cast<float>(f), 0.5f, -0.2f);
    // A box round it whose voxels' middles are not those of the domain's.
    const Vec3 origin = middle - Vec3(0.33f, 0.31f, 0.32f);
    const int n = 14;
    std::vector<float> d(static_cast<size_t>(n * n * n)), t(d.size()), fl(d.size());
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const Vec3 p = origin + (Vec3(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k)) + 0.5f) * voxel;
                const float r = length(p - middle) / 0.3f;
                const size_t at = static_cast<size_t>(i + n * (j + n * k));
                d[at] = std::max(0.0f, 1.0f - r);
                t[at] = 2.0f * d[at];
                fl[at] = f >= 3 ? 0.5f * d[at] : 0.0f;
            }
        }
    }
    std::vector<Volume> out = {Volume::make("density", origin, voxel, n, n, n, d), Volume::make("heat", origin, voxel, n, n, n, t)};
    if (f >= 3) out.push_back(Volume::make("flame", origin, voxel, n, n, n, fl));
    return out;
}

const std::string kData = PG_TEST_DATA_DIR "/vdb/";

std::vector<uint8_t> bytesOf(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

io::VdbVolumes read(const std::string& file, io::VdbReadOptions options = {}) {
    io::VdbVolumes out;
    std::string error;
    if (!io::readVdb(kData + file, out, error, options)) std::printf("    %s\n", error.c_str());
    return out;
}

const Volume* named(const io::VdbVolumes& v, const std::string& name) {
    for (const Volume& x : v.volumes) {
        if (x.name == name) return &x;
    }
    return nullptr;
}

/// The voxel of `v` a world point is in, or NaN outside it.
float voxelAt(const Volume& v, const Vec3& p) {
    const Vec3 q = (p - v.origin) / v.voxel;
    const int i = static_cast<int>(std::floor(q.x)), j = static_cast<int>(std::floor(q.y)), k = static_cast<int>(std::floor(q.z));
    if (i < 0 || j < 0 || k < 0 || i >= v.res[0] || j >= v.res[1] || k >= v.res[2]) return std::nanf("");
    return v.at(i, j, k);
}

// The smoke of make_vdb.py and make_vdb13.cpp, and where its voxel (i, j, k) is.
bool inEgg(int i, int j, int k) {
    const double x = (i - 2) / 12.0, y = (j - 10) / 10.0, z = (k - 1) / 8.0;
    return x * x + y * y + z * z <= 1.0;
}
float density(int i, int j, int k) {
    return static_cast<float>(0.5 + 0.01 * (((i * 7 + j * 13 + k * 5) % 50 + 50) % 50));
}
float temperature(int, int j, int) { return static_cast<float>(0.25 * (j - 2)); }
Vec3 smokeAt(int i, int j, int k) {
    return {static_cast<float>(0.25 + 0.1 * i), static_cast<float>(0.1 * j), static_cast<float>(-0.5 + 0.1 * k)};
}

/// How many voxels of the smoke's box `v` holds otherwise than `rule`
/// says -- every one of them, inside the egg and out.
template <typename Rule>
int smokeMisses(const Volume& v, Rule rule) {
    int misses = 0;
    for (int i = -10; i < 15; ++i) {
        for (int j = 0; j < 21; ++j) {
            for (int k = -7; k < 10; ++k) {
                const float want = inEgg(i, j, k) ? rule(i, j, k) : 0.0f;
                if (voxelAt(v, smokeAt(i, j, k)) != want) ++misses;
            }
        }
    }
    return misses;
}

}  // namespace

// --- Blosc ----------------------------------------------------------------------------

TEST(blosc_frames_decompress_as_c_blosc_made_them) {
    // python-blosc's frames (tests/data/blosc/make_blosc.py) of floats
    // ((37 i) % 101) / 4 - 3: LZ4 and BloscLZ, shuffled and split; several
    // blocks, the last one short; zlib; a frame copied as it was.
    for (const char* name : {"lz4_shuffle", "lz4_blocks", "blosclz_split", "blosclz_plain", "zlib_wide", "lz4hc_leftover"}) {
        const std::vector<uint8_t> frame = bytesOf(PG_TEST_DATA_DIR "/blosc/" + std::string(name) + ".blosc");
        CHECK(frame.size() > 16);
        std::vector<uint8_t> out;
        std::string error;
        const bool ok = io::bloscDecompress(frame, out, size_t(1) << 20, error);
        if (!ok) std::printf("    %s: %s\n", name, error.c_str());
        CHECK(ok);
        CHECK(out.size() % 4 == 0);
        int misses = 0;
        for (size_t i = 0; i < out.size() / 4; ++i) {
            float f;
            std::memcpy(&f, out.data() + 4 * i, 4);
            misses += f == static_cast<float>((37 * i) % 101) / 4.0f - 3.0f ? 0 : 1;
        }
        CHECK_EQ(misses, 0);
        // No more than may come out.
        CHECK(!io::bloscDecompress(frame, out, out.size() - 1, error));
    }
    // Cut short, or a version from after: refused.
    const std::vector<uint8_t> frame = bytesOf(PG_TEST_DATA_DIR "/blosc/lz4_blocks.blosc");
    std::vector<uint8_t> out;
    std::string error;
    for (size_t cut = 0; cut < frame.size(); cut += 7) CHECK(!io::bloscDecompress(std::span(frame.data(), cut), out, size_t(1) << 20, error));
    std::vector<uint8_t> future = frame;
    future[0] = 3;
    CHECK(!io::bloscDecompress(future, out, size_t(1) << 20, error));
    // Snappy and Zstd are not read.
    std::vector<uint8_t> zstd = frame;
    zstd[2] = static_cast<uint8_t>((4 << 5) | 0x01);
    CHECK(!io::bloscDecompress(zstd, out, size_t(1) << 20, error));
    CHECK(error.find("Zstd") != std::string::npos);
}

TEST(blosc_unshuffles_and_splits_as_c_blosc_does) {
    // One block of 512 floats split in four streams -- a byte of each float
    // apiece -- each "compressed" as it was (a stream as long as its share
    // is a copy), shuffled: what OpenVDB's leaves of 8^3 floats come out as.
    std::vector<uint8_t> values(2048);
    for (int i = 0; i < 512; ++i) {
        const float f = 0.25f * static_cast<float>(i) - 7.0f;
        std::memcpy(values.data() + 4 * i, &f, 4);
    }
    std::vector<uint8_t> frame = {2, 1, 0x01 | (1 << 5), 4, 0, 8, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0};
    const uint32_t start = 20;
    for (int b = 0; b < 4; ++b) frame.push_back(static_cast<uint8_t>(start >> (8 * b)));
    for (int s = 0; s < 4; ++s) {
        for (int b = 0; b < 4; ++b) frame.push_back(static_cast<uint8_t>(512u >> (8 * b)));
        for (int i = 0; i < 512; ++i) frame.push_back(values[static_cast<size_t>(4 * i + s)]);
    }
    const auto size = static_cast<uint32_t>(frame.size());
    for (int b = 0; b < 4; ++b) frame[static_cast<size_t>(12 + b)] = static_cast<uint8_t>(size >> (8 * b));
    std::vector<uint8_t> out;
    std::string error;
    CHECK(io::bloscDecompress(frame, out, 2048, error));
    CHECK(out == values);
}

// --- what the library writes ------------------------------------------------------------

TEST(vdb_reads_the_smoke_the_library_compressed_with_blosc) {
    // OpenVDB 10's default: Blosc's LZ4 on the active values alone.
    const io::VdbVolumes v = read("smoke.vdb");
    CHECK_EQ(v.volumes.size(), size_t(2));
    const Volume* d = named(v, "density");
    const Volume* t = named(v, "temperature");
    CHECK(d && t);
    if (!d || !t) return;
    // The box of the active voxels, (-10, 0, -7) to (14, 20, 9), as the
    // transform puts it: 0.1 m voxels whose middles are at 0.25 + 0.1 i ...
    CHECK_EQ(d->res[0], 25);
    CHECK_EQ(d->res[1], 21);
    CHECK_EQ(d->res[2], 17);
    CHECK_NEAR(d->voxel, 0.1f, 1e-7f);
    CHECK_NEAR(d->origin.x, -0.8f, 1e-6f);
    CHECK_NEAR(d->origin.y, -0.05f, 1e-6f);
    CHECK_NEAR(d->origin.z, -1.25f, 1e-6f);
    CHECK_EQ(v.classes[0], std::string("fog volume"));
    // ... and every voxel what it was made, to the bit.
    CHECK_EQ(smokeMisses(*d, density), 0);
    CHECK_EQ(smokeMisses(*t, temperature), 0);
    CHECK(v.notes.empty());
}

TEST(vdb_reads_half_floats_as_the_library_rounded_them) {
    const io::VdbVolumes v = read("smoke_half.vdb");
    const Volume* d = named(v, "density");
    CHECK(d != nullptr);
    if (!d) return;
    CHECK_EQ(smokeMisses(*d, [](int i, int j, int k) { return floatFromHalf(halfFromFloat(density(i, j, k))); }), 0);
}

TEST(vdb_reads_zip_and_uncompressed_values_alike) {
    // OpenVDB 13: zipped -- the active values alone, then every value -- a
    // stream with no offsets to its grids, and the active values as they are.
    for (const char* file : {"zip.vdb", "zip_all.vdb", "stream.vdb"}) {
        const io::VdbVolumes v = read(file);
        const Volume* d = named(v, "density");
        CHECK(d != nullptr);
        if (!d) continue;
        CHECK_EQ(smokeMisses(*d, density), 0);
        CHECK(v.notes.empty());
    }
    // The stream's second grid shares the first's tree.
    const io::VdbVolumes s = read("stream.vdb");
    const Volume* twin = named(s, "twin");
    CHECK(twin && smokeMisses(*twin, density) == 0);
}

TEST(vdb_reads_inactive_values_every_way_they_are_kept) {
    // make_vdb13.cpp's "kept": eight leaves along x, the inactive voxels of
    // each kept another way -- the background, minus it, one value, a mask
    // between two... -- a leaf with no active voxel, all 3, and the rest of
    // the box the background, 1.
    auto inactive = [](int leaf, int n) {
        switch (leaf) {
            case 0: return 1.0f;
            case 1: return -1.0f;
            case 2: return 5.0f;
            case 3: return n % 2 ? -1.0f : 1.0f;
            case 4: return n % 2 ? 7.0f : 1.0f;
            case 5: return n % 2 ? 7.0f : 9.0f;
            case 6: return static_cast<float>(n % 4);
            default: return 1.0f;
        }
    };
    for (const char* file : {"kept.vdb", "kept_raw.vdb"}) {
        const io::VdbVolumes v = read(file);
        const Volume* kept = named(v, "kept");
        CHECK(kept != nullptr);
        if (!kept) continue;
        CHECK_EQ(kept->res[0], 64);
        CHECK_EQ(kept->res[1], 16);
        CHECK_EQ(kept->res[2], 8);
        int misses = 0;
        for (int x = 0; x < 64; ++x) {
            for (int y = 0; y < 16; ++y) {
                for (int z = 0; z < 8; ++z) {
                    float want = 1.0f;
                    if (y < 8) {
                        const int leaf = x / 8, n = ((x & 7) << 6) | (y << 3) | z;
                        want = (leaf == 7 || n % 3 == 0) ? 100.0f + static_cast<float>(leaf) + static_cast<float>(n) / 1000.0f
                                                         : inactive(leaf, n);
                    } else if (x >= 8 && x < 16) {
                        want = 3.0f;
                    } else if (x == 0 && y == 15 && z == 0) {
                        want = 50.0f;
                    }
                    if (kept->at(x, y, z) != want) ++misses;
                }
            }
        }
        CHECK_EQ(misses, 0);
    }
}

TEST(vdb_reads_values_of_every_kind) {
    const io::VdbVolumes v = read("kinds.vdb");
    CHECK_EQ(v.volumes.size(), size_t(11));
    CHECK(v.notes.empty());
    struct Want {
        const char* name;
        float (*value)(int, int, int);
    };
    const Want wants[] = {
        {"double", [](int i, int j, int k) { return static_cast<float>(0.1 * i + 0.001 * j + 1e-6 * k); }},
        {"int32", [](int i, int j, int k) { return static_cast<float>(i * 100 + j * 10 + k - 50); }},
        {"int64", [](int i, int j, int k) { return static_cast<float>(int64_t(i * 100 + j * 10 + k) * 1000000000LL); }},
        {"vec3d.x", [](int i, int, int) { return static_cast<float>(0.5 * i); }},
        {"vec3d.y", [](int, int j, int) { return static_cast<float>(-0.25 * j); }},
        {"vec3d.z", [](int, int, int k) { return static_cast<float>(k); }},
        {"vec3i.x", [](int i, int, int) { return static_cast<float>(i); }},
        {"vec3i.y", [](int, int j, int) { return static_cast<float>(j); }},
        {"vec3i.z", [](int, int, int k) { return static_cast<float>(-k); }},
        {"half", [](int i, int, int) { return 0.125f * static_cast<float>(i); }},
        {"float_half",
         [](int i, int j, int) { return floatFromHalf(halfFromFloat(0.1f * static_cast<float>(i) + 0.01f * static_cast<float>(j))); }},
    };
    for (const Want& w : wants) {
        const Volume* x = named(v, w.name);
        CHECK(x != nullptr);
        if (!x) continue;
        CHECK_EQ(x->res[0], 10);
        CHECK_EQ(x->res[1], 4);
        CHECK_EQ(x->res[2], 3);
        int misses = 0;
        for (int i = 0; i < 10; ++i) {
            for (int j = 0; j < 4; ++j) {
                for (int k = 0; k < 3; ++k) misses += x->at(i, j, k) == w.value(i, j, k) ? 0 : 1;
            }
        }
        if (misses) std::printf("    %s: %d voxels wrong\n", w.name, misses);
        CHECK_EQ(misses, 0);
    }
}

TEST(vdb_tiles_fill_the_voxels_they_stand_for) {
    // A slab filled at once -- tiles of 8^3 and 128^3 voxels -- with an
    // inactive block of 2 in it and a voxel of 3 apart.
    const io::VdbVolumes v = read("tiles.vdb");
    const Volume* fill = named(v, "fill");
    CHECK(fill != nullptr);
    if (!fill) return;
    CHECK_EQ(fill->res[0], 258);
    CHECK_EQ(fill->res[1], 64);
    CHECK_EQ(fill->res[2], 16);
    double sum = 0.0;
    for (const float x : *fill->values) sum += x;
    CHECK_NEAR(sum, 0.75 * (256.0 * 64 * 16 - 512) + 2.0 * 512 + 3.0, 1e-6);
    auto at = [&](int i, int j, int k) { return voxelAt(*fill, Vec3(0.5f * static_cast<float>(i), 0.5f * static_cast<float>(j), 0.5f * static_cast<float>(k))); };
    CHECK_EQ(at(0, 0, 0), 0.75f);
    CHECK_EQ(at(-117, 44, 3), 2.0f);
    CHECK_EQ(at(-130, 5, 5), 3.0f);
    CHECK_EQ(at(-129, 5, 5), 0.0f);
    // The root's own tiles, 4096 voxels a side -- an active one of 2, an
    // inactive one of 5 beside it, a voxel of 9 in that -- averaged down
    // to fit.
    io::VdbReadOptions small;
    small.maxVoxels = 64 * 64 * 64;
    const io::VdbVolumes r = read("root.vdb", small);
    const Volume* root = named(r, "root");
    CHECK(root != nullptr);
    if (!root) return;
    CHECK(root->count() <= small.maxVoxels);
    const int f = static_cast<int>(std::lround(root->voxel));
    CHECK(f > 64);
    CHECK_EQ(r.notes.size(), size_t(1));
    // A cell inside the active tile, one inside the inactive one, the one
    // round the voxel of 9.
    CHECK_EQ(root->at(3, 3, 3), 2.0f);
    const int beyond = 4096 / f + 1;
    CHECK_EQ(root->at(beyond, 3, 3), 5.0f);
    const float nine = voxelAt(*root, Vec3(4196.0f, 5.0f, 5.0f));
    CHECK_NEAR(nine, 5.0f + 4.0f / static_cast<float>(f * f * f), 1e-5f);
}

TEST(vdb_reads_a_level_set_with_its_inside_below_zero) {
    // A ball 1 m across at (0, 0.6, 0), in voxels of 5 cm: distances within
    // three voxels of its surface, inside tiles of minus the background.
    const io::VdbVolumes v = read("sphere.vdb");
    const Volume* s = named(v, "surface");
    CHECK(s != nullptr);
    if (!s) return;
    CHECK_EQ(v.classes[0], std::string("level set"));
    CHECK_NEAR(voxelAt(*s, Vec3(0.0f, 0.6f, 0.0f)), -0.15f, 1e-6f);  // deep inside
    CHECK_NEAR(voxelAt(*s, Vec3(0.6f, 1.2f, 0.6f)), 0.15f, 1e-6f);   // well outside
    CHECK(s->sample(Vec3(0.45f, 0.6f, 0.0f)) < 0.0f);
    CHECK(s->sample(Vec3(0.55f, 0.6f, 0.0f)) > 0.0f);
    // Its surface, as a mesh: every point half a metre from the middle.
    const auto mesh = volumeToMesh(*s, 0.0f, true);
    CHECK(mesh->primitiveCount() > 500);
    float worst = 0.0f;
    for (const Vec3& p : mesh->positions()) worst = std::max(worst, std::fabs(length(p - Vec3(0.0f, 0.6f, 0.0f)) - 0.5f));
    CHECK(worst < 0.02f);
}

TEST(vdb_turned_and_stretched_grids_are_resampled_onto_cubes) {
    // Turned 30 degrees about y: 1 + 0.1 i in index space, a slope of 1 a
    // metre along the turned x. Deep in it, the resampled voxels -- and what
    // is read between them -- are that, exactly as linear as it is.
    const io::VdbVolumes v = read("turned.vdb");
    const Volume* t = named(v, "turned");
    CHECK(t != nullptr);
    if (!t) return;
    CHECK_NEAR(t->voxel, 0.1f, 1e-6f);
    CHECK_EQ(v.notes.size(), size_t(1));
    const double c = std::cos(std::numbers::pi / 6), s = std::sin(std::numbers::pi / 6);
    float worst = 0.0f;
    for (int i = 3; i <= 6; ++i) {
        for (int j = 3; j <= 4; ++j) {
            for (int k = 3; k <= 4; ++k) {
                const Vec3 p(static_cast<float>(1.0 + 0.1 * (i * c + k * s)), static_cast<float>(0.5 + 0.1 * j),
                             static_cast<float>(0.1 * (-i * s + k * c)));
                worst = std::max(worst, std::fabs(t->sample(p) - (1.0f + 0.1f * static_cast<float>(i))));
            }
        }
    }
    CHECK(worst < 1e-4f);
    // Voxels twice as tall as they are wide: vel.y is 0.2 j, the height.
    const io::VdbVolumes e = read("vel.vdb");
    const Volume* vy = named(e, "vel.y");
    const Volume* vz = named(e, "vel.z");
    CHECK(vy && vz);
    if (!vy || !vz) return;
    CHECK_NEAR(vy->voxel, 0.1f, 1e-6f);
    worst = 0.0f;
    for (int i = 0; i < 6; ++i) {
        for (int j = 1; j <= 2; ++j) {
            for (int k = 0; k < 3; ++k) {
                const Vec3 p(0.1f * static_cast<float>(i), 0.2f * static_cast<float>(j), 0.1f * static_cast<float>(k));
                worst = std::max(worst, std::fabs(vy->sample(p) - 0.2f * static_cast<float>(j)));
                worst = std::max(worst, std::fabs(vz->sample(p) + 0.3f * static_cast<float>(k)));
            }
        }
    }
    CHECK(worst < 1e-5f);
}

TEST(vdb_quarter_turns_and_mirrors_stay_voxel_for_voxel) {
    const io::VdbVolumes v = read("axes.vdb");
    CHECK(v.notes.empty());  // nothing resampled
    const Volume* quarter = named(v, "quarter");
    const Volume* mirror = named(v, "mirror");
    CHECK(quarter && mirror);
    if (!quarter || !mirror) return;
    // Mirrored in x: voxel (i, j, k) at (-1 - 0.2 i, 0.2 j, 0.5 + 0.2 k).
    int misses = 0;
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 3; ++k) {
                const Vec3 p(-1.0f - 0.2f * static_cast<float>(i), 0.2f * static_cast<float>(j), 0.5f + 0.2f * static_cast<float>(k));
                misses += voxelAt(*mirror, p) == static_cast<float>(i + 10 * j + 100 * k) ? 0 : 1;
            }
        }
    }
    CHECK_EQ(misses, 0);
    // A quarter turn about y: x along the world's z, z along its x -- the
    // voxel (0, 0, 0) where the transform moved it, (1, 2, 3).
    CHECK_EQ(quarter->res[0], 3);
    CHECK_EQ(quarter->res[1], 4);
    CHECK_EQ(quarter->res[2], 10);
    CHECK_EQ(voxelAt(*quarter, Vec3(1.0f, 2.0f, 3.0f)), 0.0f);
    // Each step along the world's axes one along the grid's.
    const float a = quarter->at(0, 0, 0), b = quarter->at(1, 0, 0), up = quarter->at(0, 1, 0), c = quarter->at(0, 0, 1);
    CHECK_EQ(std::fabs(b - a), 100.0f);
    CHECK_EQ(up - a, 10.0f);
    CHECK_EQ(std::fabs(c - a), 1.0f);
}

TEST(vdb_z_up_turns_the_world_and_its_vectors) {
    // Blender's world has z up: (x, y, z) is the program's (x, z, -y), and a
    // vector turns with it.
    io::VdbReadOptions zUp;
    zUp.zUp = true;
    const io::VdbVolumes v = read("vel.vdb", zUp);
    const Volume* vy = named(v, "vel.y");
    const Volume* vz = named(v, "vel.z");
    CHECK(vy && vz);
    if (!vy || !vz) return;
    float worst = 0.0f;
    for (int i = 0; i < 6; ++i) {
        for (int j = 1; j <= 2; ++j) {
            for (int k = 0; k < 3; ++k) {
                const Vec3 p(0.1f * static_cast<float>(i), 0.1f * static_cast<float>(k), -0.2f * static_cast<float>(j));
                worst = std::max(worst, std::fabs(vy->sample(p) + 0.3f * static_cast<float>(k)));  // the file's z
                worst = std::max(worst, std::fabs(vz->sample(p) + 0.2f * static_cast<float>(j)));  // minus its y
            }
        }
    }
    CHECK(worst < 1e-5f);
}

TEST(vdb_grids_not_read_are_notes_and_instances_share_trees) {
    // A bool grid and one in a camera's frustum beside floats -- one of
    // them an instance of another's tree, moved by its own transform -- and
    // two grids of one name.
    const io::VdbVolumes v = read("mixed.vdb");
    CHECK_EQ(v.volumes.size(), size_t(3));
    CHECK_EQ(v.notes.size(), size_t(2));
    const Volume* b = named(v, "b");
    CHECK(b != nullptr);
    if (b) {
        CHECK_NEAR(b->voxel, 2.0f, 1e-6f);
        CHECK_EQ(voxelAt(*b, Vec3(8.0f, 10.0f, 12.0f)), 0.5f);
    }
    int as = 0;
    for (const Volume& x : v.volumes) as += x.name == "a" ? 1 : 0;
    CHECK_EQ(as, 2);
    // By name: the one asked for -- both of that name -- and a note for one
    // that is not there.
    io::VdbReadOptions only;
    only.grids = {"a", "nothing"};
    const io::VdbVolumes a = read("mixed.vdb", only);
    CHECK_EQ(a.volumes.size(), size_t(2));
    CHECK_EQ(a.notes.size(), size_t(1));
    CHECK(a.notes.size() == 1 && a.notes[0].find("nothing") != std::string::npos);
}

TEST(vdb_grids_lists_what_is_in_a_file_from_its_headers) {
    std::vector<io::VdbGridInfo> grids;
    std::string error;
    CHECK(io::vdbGrids(kData + "smoke.vdb", grids, error));
    CHECK_EQ(grids.size(), size_t(2));
    if (grids.size() == 2) {
        CHECK_EQ(grids[0].name, std::string("density"));
        CHECK_EQ(grids[0].type, std::string("float"));
        CHECK_EQ(grids[0].gridClass, std::string("fog volume"));
        CHECK(grids[0].readable && grids[0].bounded);
        CHECK_NEAR(grids[0].voxel, 0.1f, 1e-7f);
        CHECK_NEAR(grids[0].lo.x, -0.8f, 1e-5f);
        CHECK_NEAR(grids[0].hi.y, 2.05f, 1e-5f);
    }
    CHECK(io::vdbGrids(kData + "mixed.vdb", grids, error));
    CHECK_EQ(grids.size(), size_t(5));
    if (grids.size() == 5) {
        CHECK(!grids[0].readable);  // bool
        CHECK(!grids[1].readable);  // a frustum
        CHECK(grids[2].readable);
    }
    CHECK(io::vdbGrids(kData + "stream.vdb", grids, error));
    CHECK_EQ(grids.size(), size_t(3));
    CHECK(!io::vdbGrids(kData + "nothing.vdb", grids, error));
}

TEST(vdb_large_grids_are_averaged_down_to_fit) {
    io::VdbReadOptions small;
    small.maxVoxels = 2000;
    small.grids = {"density"};
    const io::VdbVolumes v = read("smoke.vdb", small);
    const Volume* d = named(v, "density");
    CHECK(d != nullptr);
    if (!d) return;
    CHECK(d->count() <= 2000);
    CHECK_EQ(v.notes.size(), size_t(1));
    const int f = static_cast<int>(std::lround(d->voxel / 0.1f));
    CHECK(f >= 2);
    CHECK_NEAR(d->voxel, 0.1f * static_cast<float>(f), 1e-6f);
    // Each voxel the mean of the f^3 it stands for.
    float worst = 0.0f;
    for (int x = 0; x < d->res[0]; ++x) {
        for (int y = 0; y < d->res[1]; ++y) {
            for (int z = 0; z < d->res[2]; ++z) {
                double sum = 0.0;
                for (int a = 0; a < f; ++a) {
                    for (int b = 0; b < f; ++b) {
                        for (int c = 0; c < f; ++c) {
                            const int i = -10 + x * f + a, j = y * f + b, k = -7 + z * f + c;
                            sum += inEgg(i, j, k) ? density(i, j, k) : 0.0f;
                        }
                    }
                }
                worst = std::max(worst, std::fabs(d->at(x, y, z) - static_cast<float>(sum / (f * f * f))));
            }
        }
    }
    CHECK(worst < 1e-5f);
}

TEST(vdb_reads_what_our_own_writer_writes) {
    std::vector<float> values(20 * 12 * 9, 0.0f);
    for (int k = 0; k < 9; ++k) {
        for (int j = 0; j < 12; ++j) {
            for (int i = 0; i < 20; ++i) {
                if ((i * 3 + j * 5 + k) % 7 < 3 || (i > 4 && i < 9)) {
                    values[static_cast<size_t>(i + 20 * (j + 12 * k))] = 0.1f * static_cast<float>(i - 6) + 0.01f * static_cast<float>(j * k);
                }
            }
        }
    }
    const Volume in = Volume::make("density", Vec3(-1.0f, 0.25f, 2.0f), 0.05f, 20, 12, 9, values);
    const std::string bytes = io::formatVdb({in});
    io::VdbVolumes out;
    std::string error;
    CHECK(io::parseVdb(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), out, error));
    CHECK_EQ(out.volumes.size(), size_t(1));
    if (out.volumes.empty()) return;
    const Volume& v = out.volumes[0];
    CHECK_NEAR(v.voxel, 0.05f, 1e-7f);
    int misses = 0;
    for (int k = 0; k < 9; ++k) {
        for (int j = 0; j < 12; ++j) {
            for (int i = 0; i < 20; ++i) {
                const Vec3 p = in.origin + (Vec3(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k)) + 0.5f) * in.voxel;
                const float got = voxelAt(v, p);
                const float want = in.at(i, j, k);
                // The box of the voxels that are not 0: outside it, nothing.
                if (std::isnan(got) ? want != 0.0f : got != want) ++misses;
            }
        }
    }
    CHECK_EQ(misses, 0);
}

TEST(vdb_broken_files_are_refused_not_crashed_on) {
    io::VdbReadOptions small;
    small.maxVoxels = 1 << 14;
    for (const char* file : {"smoke.vdb", "zip.vdb", "kept.vdb", "stream.vdb", "mixed.vdb", "tiles.vdb"}) {
        const std::vector<uint8_t> whole = bytesOf(kData + file);
        CHECK(whole.size() > 1000);
        // Cut short anywhere: refused.
        int read = 0;
        for (size_t cut = 0; cut < whole.size(); cut += whole.size() / 37 + 1) {
            io::VdbVolumes out;
            std::string error;
            if (io::parseVdb(std::span(whole.data(), cut), out, error, small)) ++read;
        }
        CHECK_EQ(read, 0);
        // A byte changed anywhere: read or refused, never a crash.
        for (size_t at = 0; at < whole.size(); at += whole.size() / 97 + 1) {
            for (const uint8_t flip : {uint8_t(0x01), uint8_t(0xff)}) {
                std::vector<uint8_t> broken = whole;
                broken[at] ^= flip;
                io::VdbVolumes out;
                std::string error;
                io::parseVdb(broken, out, error, small);
            }
        }
    }
    io::VdbVolumes out;
    std::string error;
    CHECK(!io::readVdb(kData + "nothing.vdb", out, error));
    const std::string text = "not a vdb at all, but long enough to be read as a header and more";
    CHECK(!io::parseVdb(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), out, error));
    CHECK(error.find("not an OpenVDB file") != std::string::npos);
}

// --- the nodes --------------------------------------------------------------------------

TEST(vdb_import_node_reads_volumes_and_their_surfaces) {
    sim::Network net;
    const int n = net.add("vdb_import");
    CHECK(net.setText(n, "file", kData + "smoke.vdb"));
    sim::GeometryGraph graph;
    graph.sync(net);
    const GeometryPtr volumes = graph.cook(n, 1, 1.0f / 30.0f);
    CHECK_EQ(volumes->volumeCount(), size_t(2));
    CHECK(volumes->findVolume("density") && volumes->findVolume("temperature"));
    CHECK_EQ(graph.error(n), std::string());
    // By name; and a level set's surface, where it crosses 0 -- a ball.
    CHECK(net.setText(n, "grids", "temperature"));
    graph.sync(net);
    CHECK_EQ(graph.cook(n, 1, 1.0f / 30.0f)->volumeCount(), size_t(1));
    CHECK(net.setText(n, "file", kData + "sphere.vdb"));
    CHECK(net.setText(n, "grids", ""));
    net.setParam(n, "surface", "1");
    graph.sync(net);
    const GeometryPtr ball = graph.cook(n, 1, 1.0f / 30.0f);
    CHECK_EQ(ball->volumeCount(), size_t(0));
    CHECK(ball->primitiveCount() > 500);
    Vec3 lo(1e9f), hi(-1e9f);
    for (const Vec3& p : ball->positions()) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    CHECK(length(lo - Vec3(-0.5f, 0.1f, -0.5f)) < 0.03f);
    CHECK(length(hi - Vec3(0.5f, 1.1f, 0.5f)) < 0.03f);
    // Notes are warnings; a file that is not there, an error.
    CHECK(net.setText(n, "file", kData + "mixed.vdb"));
    net.setParam(n, "surface", "0");
    graph.sync(net);
    graph.cook(n, 1, 1.0f / 30.0f);
    CHECK(graph.warning(n).find("bool") != std::string::npos);
    CHECK(net.setText(n, "file", kData + "nowhere.vdb"));
    graph.sync(net);
    CHECK_EQ(graph.cook(n, 1, 1.0f / 30.0f)->volumeCount(), size_t(0));
    CHECK(graph.error(n).find("nowhere.vdb") != std::string::npos);
}

TEST(vdb_import_node_plays_a_numbered_sequence) {
    TempFolder dir("vdbseq");
    for (int f = 1; f <= 4; ++f) {
        std::string error;
        CHECK(io::writeVdb(puff(f), dir / ("puff." + std::to_string(1000 + f) + ".vdb"), error));
    }
    sim::Network net;
    const int n = net.add("vdb_import");
    CHECK(net.setText(n, "file", dir / "puff.####.vdb"));
    net.setParam(n, "offset", "1000");
    sim::GeometryGraph graph;
    graph.sync(net);
    // Frame f reads file 1000 + f: the ball where frame f put it.
    for (int f = 1; f <= 4; ++f) {
        const GeometryPtr g = graph.cook(n, f, 1.0f / 30.0f);
        const Volume* d = g->findVolume("density");
        CHECK(d != nullptr);
        if (!d) continue;
        CHECK_NEAR(d->sample(Vec3(0.1f * static_cast<float>(f), 0.5f, -0.2f)), 1.0f, 0.1f);
    }
    // A frame with no file: nothing, and a warning.
    const GeometryPtr none = graph.cook(n, 9, 1.0f / 30.0f);
    CHECK_EQ(none->volumeCount(), size_t(0));
    CHECK(graph.warning(n).find("no file for frame") != std::string::npos);
    CHECK_EQ(graph.error(n), std::string());
}

TEST(vdb_gas_lays_every_frame_on_one_domain_voxel_for_voxel) {
    TempFolder dir("vdbgas");
    for (int f = 1; f <= 6; ++f) {
        std::string error;
        CHECK(io::writeVdb(puff(f), dir / ("puff_" + std::to_string(f) + ".vdb"), error));
    }
    sim::VdbGas g;
    g.file = dir / "puff_$F.vdb";
    sim::Domain d;
    int factor = 0;
    std::vector<std::string> notes;
    std::string error;
    CHECK(sim::vdbGasDomain(g, 6, d, factor, notes, error));
    CHECK_EQ(factor, 1);
    CHECK_NEAR(d.voxel, 0.05f, 1e-6f);
    CHECK(notes.empty());
    CHECK(!g.stamp.empty());
    // Every frame's ball in it -- its voxels from x = -0.22 to 0.92, up to
    // y = 0.79 -- round the y axis.
    const Vec3 lo = d.origin(), hi = d.origin() + d.size();
    CHECK(lo.x <= -0.92f && hi.x >= 0.92f && lo.y == 0.0f && hi.y >= 0.79f && lo.z <= -0.52f);
    CHECK(d.cells[0] % 8 == 0 && d.cells[1] % 8 == 0 && d.cells[2] % 8 == 0);
    // Frame 3: each voxel on the cell its middle is nearest -- its value,
    // as a half -- the rest of the domain empty.
    sim::Frame frame;
    CHECK(sim::vdbGasFrame(g, d, 3, frame, error));
    CHECK(!frame.gasTiles.empty() && frame.gasTiles.size() < static_cast<size_t>(d.cells[0] * d.cells[1] * d.cells[2]) / 512);
    const std::vector<Volume> made = puff(3);
    int misses = 0, cells = 0;
    double sum = 0.0, want = 0.0;
    for (int k = 0; k < 14; ++k) {
        for (int j = 0; j < 14; ++j) {
            for (int i = 0; i < 14; ++i) {
                const Vec3 p = made[0].origin + (Vec3(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k)) + 0.5f) * 0.05f;
                const Vec3 q = (p - lo) / d.voxel;
                const int ci = static_cast<int>(std::floor(q.x)), cj = static_cast<int>(std::floor(q.y)), ck = static_cast<int>(std::floor(q.z));
                ++cells;
                misses += frame.at(0, ci, cj, ck) == floatFromHalf(halfFromFloat(made[0].at(i, j, k))) ? 0 : 1;
                misses += frame.at(1, ci, cj, ck) == floatFromHalf(halfFromFloat(made[1].at(i, j, k))) ? 0 : 1;
                misses += frame.at(2, ci, cj, ck) == floatFromHalf(halfFromFloat(made[2].at(i, j, k))) ? 0 : 1;
                want += made[0].at(i, j, k);
            }
        }
    }
    CHECK_EQ(misses, 0);
    for (int k = 0; k < d.cells[2]; ++k) {
        for (int j = 0; j < d.cells[1]; ++j) {
            for (int i = 0; i < d.cells[0]; ++i) sum += frame.at(0, i, j, k);
        }
    }
    CHECK_NEAR(sum, want, 0.01 * want);
    CHECK(frame.steam.empty());
    // A frame with no file has no gas; a coarser domain -- a preview's --
    // has what the files hold, averaged.
    CHECK(sim::vdbGasFrame(g, d, 9, frame, error));
    CHECK_EQ(frame.gasTiles.size(), size_t(1));
    CHECK(std::all_of(frame.fields.begin(), frame.fields.end(), [](uint16_t h) { return h == 0; }));
    sim::Domain coarse = d;
    coarse.voxel *= 2.0f;
    for (int& c : coarse.cells) c = std::max(8, c / 2 / 8 * 8);
    CHECK(sim::vdbGasFrame(g, coarse, 3, frame, error));
    double coarseSum = 0.0;
    for (int k = 0; k < coarse.cells[2]; ++k) {
        for (int j = 0; j < coarse.cells[1]; ++j) {
            for (int i = 0; i < coarse.cells[0]; ++i) coarseSum += frame.at(0, i, j, k);
        }
    }
    CHECK_NEAR(coarseSum * 8.0, want, 0.05 * want);
    (void)cells;
}

TEST(vdb_gas_is_the_volume_looks_gas_frame_by_frame) {
    TempFolder dir("vdbshot");
    for (int f = 1; f <= 5; ++f) {
        std::string error;
        CHECK(io::writeVdb(puff(f), dir / ("puff_" + std::to_string(f) + ".vdb"), error));
    }
    sim::Network net;
    const int gas = net.add("vdb_gas");
    const int look = net.add("volume_look");
    const int out = net.add("output");
    CHECK(net.setText(gas, "file", "puff_$F.vdb"));  // beside the network
    CHECK(net.setText(gas, "temperature", "temperature heat"));
    net.setParam(out, "frames", "5");
    CHECK(net.connect(gas, "gas", look, "gas"));
    CHECK(net.connect(look, "look", out, "look"));
    const int volume = net.add("gas_volume");
    CHECK(net.connect(gas, "gas", volume, "gas"));
    const sim::Compiled c = net.compile(dir.path.string());
    for (const sim::Problem& p : c.problems) std::printf("    %s\n", p.message.c_str());
    CHECK(c.ok);
    CHECK(c.world.hasGas && c.world.vdbGas.any());
    CHECK_EQ(c.solver, gas);
    // Simulated, it is read: frame f's ball, as hot, burning from frame 3.
    sim::WorldSolver solver(c.world);
    CHECK(solver.gas() == nullptr);
    for (int f = 1; f <= 5; ++f) {
        solver.step();
        const sim::Frame frame = solver.capture();
        CHECK_EQ(solver.gasFileError(), std::string());
        CHECK_EQ(frame.number, f);
        const sim::Domain& d = frame.domain;
        const Vec3 q = (Vec3(0.1f * static_cast<float>(f), 0.5f, -0.2f) - d.origin()) / d.voxel;
        const int i = static_cast<int>(q.x), j = static_cast<int>(q.y), k = static_cast<int>(q.z);
        CHECK(frame.at(0, i, j, k) > 0.8f);
        CHECK_NEAR(frame.at(1, i, j, k), 2.0f * frame.at(0, i, j, k), 0.01f);
        CHECK((frame.at(2, i, j, k) > 0.0f) == (f >= 3));
    }
    // Its files changed: another world.
    {
        std::string error;
        std::vector<Volume> bigger = puff(2);
        bigger.pop_back();
        CHECK(io::writeVdb(bigger, dir / "puff_2.vdb", error));
    }
    CHECK(!(net.compile(dir.path.string()).world == c.world));
    // No files: an error on the node.
    CHECK(net.setText(gas, "file", "nothing_$F.vdb"));
    bool said = false;
    for (const sim::Problem& p : net.compile(dir.path.string()).problems) said = said || (p.node == gas && p.message.find("no files") != std::string::npos);
    CHECK(said);
}

TEST(vdb_gas_plays_back_the_gas_it_was_exported_from_to_the_bit) {
    // A fire simulated, its gas out to OpenVDB a frame a file -- as Gas
    // Volume and --export write it -- and read back as VDB Gas: every cell
    // the same half it was, and so how fast it goes, block by block.
    sim::Scene scene = sim::Scene::fire();
    scene.solver.resolution = 32;
    sim::PyroSolver solver(scene);
    TempFolder dir("vdbround");
    std::vector<sim::Frame> made;
    for (int f = 1; f <= 6; ++f) {
        solver.step();
        made.push_back(sim::capture(solver));
        sim::addVelocity(made.back(), solver);
        CHECK(!made.back().velocity.empty());
        std::string error;
        CHECK(io::writeVdb(sim::gasVolumes(made.back()), dir / ("fire." + std::to_string(f) + ".vdb"), error));
    }
    sim::VdbGas g;
    g.file = dir / "fire.$F.vdb";
    sim::Domain d;
    int factor = 0;
    std::vector<std::string> notes;
    std::string error;
    CHECK(sim::vdbGasDomain(g, 6, d, factor, notes, error));
    CHECK_EQ(factor, 1);
    CHECK_EQ(d.voxel, made[0].domain.voxel);
    int misses = 0, cells = 0, lit = 0, blocks = 0, slower = 0;
    for (int f = 1; f <= 6; ++f) {
        sim::Frame played;
        CHECK(sim::vdbGasFrame(g, d, f, played, error));
        const sim::Frame& was = made[static_cast<size_t>(f - 1)];
        const sim::Domain& w = was.domain;
        // Each block played whose middle is one of the simulation's.
        std::vector<uint16_t> a, b;
        const std::vector<uint16_t>& got = played.denseVelocity(a);
        const std::vector<uint16_t>& want = was.denseVelocity(b);
        CHECK(!got.empty() && !want.empty());
        const float edge = 2.0f * d.voxel;
        for (int k = 0; k < played.blocks(2) && !got.empty() && !want.empty(); ++k) {
            for (int j = 0; j < played.blocks(1); ++j) {
                for (int i = 0; i < played.blocks(0); ++i) {
                    const Vec3 middle = d.origin() + (Vec3(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k)) + 0.5f) * edge;
                    const Vec3 q = (middle - w.origin()) / edge - 0.5f;
                    const Vec3 r = glm::round(q);
                    if (length(q - r) > 1e-3f || r.x < 0.0f || r.y < 0.0f || r.z < 0.0f || r.x >= static_cast<float>(was.blocks(0)) ||
                        r.y >= static_cast<float>(was.blocks(1)) || r.z >= static_cast<float>(was.blocks(2))) {
                        continue;
                    }
                    const size_t from = static_cast<size_t>(r.x) + static_cast<size_t>(was.blocks(0)) *
                                            (static_cast<size_t>(r.y) + static_cast<size_t>(was.blocks(1)) * static_cast<size_t>(r.z));
                    const size_t to = static_cast<size_t>(i) + static_cast<size_t>(played.blocks(0)) *
                                          (static_cast<size_t>(j) + static_cast<size_t>(played.blocks(1)) * static_cast<size_t>(k));
                    for (int c = 0; c < 3; ++c) slower += got[3 * to + c] == want[3 * from + c] ? 0 : 1;
                    ++blocks;
                }
            }
        }
        for (int k = 0; k < w.cells[2]; ++k) {
            for (int j = 0; j < w.cells[1]; ++j) {
                for (int i = 0; i < w.cells[0]; ++i) {
                    const Vec3 c = w.origin() + (Vec3(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k)) + 0.5f) * w.voxel;
                    const Vec3 q = (c - d.origin()) / d.voxel;
                    const int a = static_cast<int>(std::floor(q.x)), b = static_cast<int>(std::floor(q.y)), e = static_cast<int>(std::floor(q.z));
                    const bool in = a >= 0 && b >= 0 && e >= 0 && a < d.cells[0] && b < d.cells[1] && e < d.cells[2];
                    for (int ch = 0; ch < 3; ++ch) {
                        const float want = was.at(ch, i, j, k);
                        const float got = in ? played.at(ch, a, b, e) : 0.0f;
                        misses += got == want ? 0 : 1;
                        lit += want > 0.0f ? 1 : 0;
                    }
                    ++cells;
                }
            }
        }
    }
    CHECK(lit > 1000);
    CHECK_EQ(misses, 0);
    CHECK(blocks > 1000);
    CHECK_EQ(slower, 0);
    (void)cells;
}
