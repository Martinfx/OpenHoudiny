// The OpenVDB files tests/test_vdb_read.cpp reads that make_vdb.py cannot
// make -- written by OpenVDB 13 itself (file version 225), built from the
// sources the build fetches, without Blosc:
//
//   cmake build/_deps/openvdb-src -DOPENVDB_BUILD_BINARIES=OFF \
//       -DOPENVDB_BUILD_PYTHON_MODULE=OFF -DOPENVDB_BUILD_UNITTESTS=OFF \
//       -DOPENVDB_BUILD_NANOVDB=OFF -DUSE_BLOSC=OFF -DUSE_ZLIB=ON \
//       -DOPENVDB_USE_DELAYED_LOADING=OFF -DOPENVDB_CORE_STATIC=ON \
//       -DOPENVDB_CORE_SHARED=OFF   (in a folder of its own, then make openvdb_static)
//   g++ -std=c++17 -O1 make_vdb13.cpp -I<that>/openvdb/openvdb \
//       -Ibuild/_deps/openvdb-src/openvdb <that>/openvdb/openvdb/libopenvdb.a \
//       -ltbb -lz -lpthread -o make_vdb13
//   ./make_vdb13 tests/data/vdb
//
// Each grid's values follow a rule the test follows again, voxel by voxel.
#include <openvdb/openvdb.h>
#include <openvdb/io/Stream.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>

using namespace openvdb;

namespace {

std::string folder;

void write(const std::string& name, const GridPtrVec& grids, uint32_t compression) {
    io::File file(folder + "/" + name);
    file.setCompression(compression);
    file.write(grids);
    file.close();
}

// The smoke of make_vdb.py: an egg round voxel (2, 10, 1).
bool inside(int i, int j, int k) {
    const double x = (i - 2) / 12.0, y = (j - 10) / 10.0, z = (k - 1) / 8.0;
    return x * x + y * y + z * z <= 1.0;
}
double density(int i, int j, int k) { return 0.5 + 0.01 * (((i * 7 + j * 13 + k * 5) % 50 + 50) % 50); }

// A fog volume -- or, as OpenVDB zips no fog volume and no level set, of
// no class.
FloatGrid::Ptr smoke(bool fog) {
    auto g = FloatGrid::create(0.0f);
    g->setName("density");
    if (fog) g->setGridClass(GRID_FOG_VOLUME);
    auto t = math::Transform::createLinearTransform(0.1);
    t->postTranslate(Vec3d(0.25, 0.0, -0.5));
    g->setTransform(t);
    auto a = g->getAccessor();
    for (int i = -10; i < 15; ++i) {
        for (int j = 0; j < 21; ++j) {
            for (int k = -7; k < 10; ++k) {
                if (inside(i, j, k)) a.setValueOn(Coord(i, j, k), static_cast<float>(density(i, j, k)));
            }
        }
    }
    return g;
}

// Each leaf along x with its inactive voxels kept otherwise -- every way
// the library writes what they are (readCompressedValues' metadata) --
// over a background of 1. Voxel n of leaf L is active when n % 3 == 0, at
// 100 + L + n / 1000.
float inactive(int leaf, int n) {
    switch (leaf) {
        case 0: return 1.0f;                       // all the background: NO_MASK_OR_INACTIVE_VALS
        case 1: return -1.0f;                      // minus it: NO_MASK_AND_MINUS_BG
        case 2: return 5.0f;                       // another: NO_MASK_AND_ONE_INACTIVE_VAL
        case 3: return n % 2 ? -1.0f : 1.0f;       // either: MASK_AND_NO_INACTIVE_VALS
        case 4: return n % 2 ? 7.0f : 1.0f;        // another or the background: MASK_AND_ONE_INACTIVE_VAL
        case 5: return n % 2 ? 7.0f : 9.0f;        // two others: MASK_AND_TWO_INACTIVE_VALS
        case 6: return static_cast<float>(n % 4);  // more: NO_MASK_AND_ALL_VALS
        default: return 1.0f;                      // 7: every voxel active
    }
}

FloatGrid::Ptr kept() {
    auto g = FloatGrid::create(1.0f);
    g->setName("kept");
    auto a = g->getAccessor();
    for (int leaf = 0; leaf < 8; ++leaf) {
        for (int n = 0; n < 512; ++n) {
            const Coord c(8 * leaf + (n >> 6), (n >> 3) & 7, n & 7);
            if (leaf == 7 || n % 3 == 0) a.setValueOn(c, 100.0f + leaf + n / 1000.0f);
            else a.setValueOff(c, inactive(leaf, n));
        }
    }
    // A leaf with no active voxel, all 3 -- inside the box of those that
    // are: one at (0, 15, 0).
    for (int n = 0; n < 512; ++n) a.setValueOff(Coord(8 + (n >> 6), 8 + ((n >> 3) & 7), n & 7), 3.0f);
    a.setValueOn(Coord(0, 15, 0), 50.0f);
    return g;
}

template <typename GridT, typename F>
typename GridT::Ptr box(const std::string& name, F value) {
    auto g = GridT::create();
    g->setName(name);
    g->setTransform(math::Transform::createLinearTransform(0.5));
    auto a = g->getAccessor();
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 3; ++k) a.setValueOn(Coord(i, j, k), value(i, j, k));
        }
    }
    return g;
}

}  // namespace

int main(int argc, char** argv) {
    folder = argc > 1 ? argv[1] : ".";
    initialize();

    // The smoke zipped -- the active values alone, and every value.
    write("zip.vdb", {smoke(false)}, io::COMPRESS_ZIP | io::COMPRESS_ACTIVE_MASK);
    write("zip_all.vdb", {smoke(false)}, io::COMPRESS_ZIP);

    // Inactive values kept every way, zipped and not.
    write("kept.vdb", {kept()}, io::COMPRESS_ZIP | io::COMPRESS_ACTIVE_MASK);
    write("kept_raw.vdb", {kept()}, io::COMPRESS_ACTIVE_MASK);

    // Values of every kind read: 10 x 4 x 3 voxels of half a metre.
    GridPtrVec kinds;
    kinds.push_back(box<DoubleGrid>("double", [](int i, int j, int k) { return 0.1 * i + 0.001 * j + 1e-6 * k; }));
    kinds.push_back(box<Int32Grid>("int32", [](int i, int j, int k) { return i * 100 + j * 10 + k - 50; }));
    kinds.push_back(box<Int64Grid>("int64", [](int i, int j, int k) { return int64_t(i * 100 + j * 10 + k) * 1000000000LL; }));
    kinds.push_back(box<Vec3DGrid>("vec3d", [](int i, int j, int k) { return Vec3d(0.5 * i, -0.25 * j, k); }));
    kinds.push_back(box<Vec3IGrid>("vec3i", [](int i, int j, int k) { return Vec3i(i, j, -k); }));
    kinds.push_back(box<HalfGrid>("half", [](int i, int, int) { return math::half(0.125f * static_cast<float>(i)); }));
    auto asHalf = box<FloatGrid>("float_half", [](int i, int j, int) { return 0.1f * static_cast<float>(i) + 0.01f * static_cast<float>(j); });
    asHalf->setSaveFloatAsHalf(true);
    kinds.push_back(asHalf);
    write("kinds.vdb", kinds, io::COMPRESS_ZIP | io::COMPRESS_ACTIVE_MASK);

    // Turned by a quarter about y, and mirrored in x: still voxel for voxel.
    auto quarter = box<FloatGrid>("quarter", [](int i, int j, int k) { return static_cast<float>(i + 10 * j + 100 * k); });
    auto qt = math::Transform::createLinearTransform(0.2);
    qt->postRotate(M_PI / 2, math::Y_AXIS);
    qt->postTranslate(Vec3d(1.0, 2.0, 3.0));
    quarter->setTransform(qt);
    auto mirror = box<FloatGrid>("mirror", [](int i, int j, int k) { return static_cast<float>(i + 10 * j + 100 * k); });
    math::Mat4d m = math::Mat4d::identity();
    m.setToScale(Vec3d(-0.2, 0.2, 0.2));
    m.setTranslation(Vec3d(-1.0, 0.0, 0.5));
    mirror->setTransform(math::Transform::createLinearTransform(m));
    write("axes.vdb", {quarter, mirror}, io::COMPRESS_ZIP | io::COMPRESS_ACTIVE_MASK);

    // Tiles of the root, 4096 voxels a side: one active, one not, and a
    // voxel beyond them.
    {
        auto root = FloatGrid::create(0.0f);
        root->setName("root");
        root->fill(CoordBBox(Coord(0), Coord(4095)), 2.0f, true);
        root->fill(CoordBBox(Coord(4096, 0, 0), Coord(8191, 4095, 4095)), 5.0f, false);
        root->getAccessor().setValueOn(Coord(4196, 5, 5), 9.0f);
        write("root.vdb", {root}, io::COMPRESS_ZIP | io::COMPRESS_ACTIVE_MASK);
    }

    // A stream: no offsets to the grids -- each read to its end to find the
    // next. The smoke, an instance of it, and half floats.
    {
        auto d = smoke(true);
        auto twin = d->copyGrid();
        twin->setName("twin");
        std::ofstream os(folder + "/stream.vdb", std::ios::binary);
        io::Stream(os).write({d, twin, kinds[5]});
    }
    return 0;
}
