//
// Shots out to the other programs and to disk: PLY points (src/pg/io/Ply.h),
// OpenVDB volumes (Vdb.h), writing by extension and numbering the files of a
// sequence (Export.h) -- and the frames of a simulation cached in a folder
// (src/pg/sim/Cache.h).
//
#include "pg/io/Export.h"
#include "pg/io/Ply.h"
#include "pg/io/Vdb.h"
#include "pg/sim/Cache.h"
#include "pg/sim/Network.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
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

std::string fileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool near(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) <= eps; }
bool near(const Vec3& a, const Vec3& b, float eps = 1e-6f) { return length(a - b) <= eps; }

/// Four points with every kind of attribute, a triangle, a quad and a line.
Geometry everything() {
    Geometry geo;
    geo.addPoints(4);
    auto p = geo.positionsForWrite();
    p[0] = {0.0f, 0.0f, 0.0f};
    p[1] = {1.0f, 0.0f, 0.0f};
    p[2] = {1.0f, 1.0f, -0.5f};
    p[3] = {0.0f, 1.0f, 0.25f};
    auto n = geo.points().create("N", AttrType::Vec3).write<Vec3>();
    auto cd = geo.points().create("Cd", AttrType::Vec3).write<Vec3>();
    auto v = geo.points().create("v", AttrType::Vec3).write<Vec3>();
    auto pscale = geo.points().create("pscale", AttrType::Float).write<float>();
    auto id = geo.points().create("id", AttrType::Int).write<int32_t>();
    auto uv = geo.points().create("uv", AttrType::Vec2).write<Vec2>();
    auto orient = geo.points().create("orient", AttrType::Vec4).write<Vec4>();
    geo.points().create("name", AttrType::String);
    for (size_t i = 0; i < 4; ++i) {
        const float f = static_cast<float>(i);
        n[i] = {0.0f, 0.0f, 1.0f};
        cd[i] = {f * 51.0f / 255.0f, 1.0f, 0.0f};  // bytes: 0, 51, 102, 153 -- exact
        v[i] = {f, -2.0f * f, 0.5f};
        pscale[i] = 0.1f + f;
        id[i] = 16777217 + static_cast<int32_t>(i);  // not a float's: 2^24 + 1
        uv[i] = {f * 0.25f, 1.0f - f * 0.25f};
        orient[i] = {0.0f, 0.0f, f, 1.0f};
    }
    const uint32_t tri[3] = {0, 1, 2}, quad[4] = {0, 1, 2, 3}, line[2] = {1, 3};
    geo.addPrimitive(tri, true);
    geo.addPrimitive(quad, true);
    geo.addPrimitive(line, false);  // no face: left out of PLY
    return geo;
}

/// Reads what writeGrid() and formatVdb() write -- the file's framing, not
/// the trees -- as OpenVDB does: the header, the file's metadata, and each
/// grid's descriptor and where it says it starts and ends.
struct VdbFile {
    struct Grid {
        std::string name, type;
        int64_t gridPos = 0, blockPos = 0, endPos = 0;
        std::map<std::string, std::string> meta;  ///< metadata values, raw
    };
    bool ok = true;
    std::string uuid;
    uint32_t version = 0;
    std::map<std::string, std::string> fileMeta;
    std::vector<Grid> grids;

    explicit VdbFile(const std::string& d) : data(d) {
        if (i64() != 0x56444220) ok = false;
        version = u32();
        u32();
        u32();
        if (u8() != 1) ok = false;  // offsets
        uuid = raw(36);
        readMeta(fileMeta);
        const int32_t count = static_cast<int32_t>(u32());
        for (int32_t g = 0; g < count && ok; ++g) {
            Grid grid;
            grid.name = string();
            grid.type = string();
            string();  // the parent
            grid.gridPos = i64();
            grid.blockPos = i64();
            grid.endPos = i64();
            if (static_cast<int64_t>(at) != grid.gridPos) ok = false;
            if (u32() != 2) ok = false;  // active-mask compression
            readMeta(grid.meta);
            if (grid.endPos < grid.blockPos || grid.blockPos < grid.gridPos || grid.endPos > static_cast<int64_t>(data.size())) {
                ok = false;
                break;
            }
            at = static_cast<size_t>(grid.endPos);
            grids.push_back(grid);
        }
        if (at != data.size()) ok = false;
    }

    const std::string& data;
    size_t at = 0;

    uint64_t bits(int n) {
        if (at + static_cast<size_t>(n) > data.size()) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(data[at + static_cast<size_t>(i)])) << (8 * i);
        at += static_cast<size_t>(n);
        return v;
    }
    uint8_t u8() { return static_cast<uint8_t>(bits(1)); }
    uint32_t u32() { return static_cast<uint32_t>(bits(4)); }
    int64_t i64() { return static_cast<int64_t>(bits(8)); }
    std::string raw(size_t n) {
        if (at + n > data.size()) {
            ok = false;
            return {};
        }
        std::string s = data.substr(at, n);
        at += n;
        return s;
    }
    std::string string() { return raw(u32()); }
    void readMeta(std::map<std::string, std::string>& out) {
        const uint32_t n = u32();
        for (uint32_t i = 0; i < n && ok; ++i) {
            const std::string name = string();
            string();  // the type
            out[name] = string();
        }
    }
};

int64_t int64Of(const std::string& bytes) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && i < bytes.size(); ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
    return static_cast<int64_t>(v);
}

bool sameFrame(const sim::Frame& a, const sim::Frame& b) {
    const sim::WaterFrame &w = a.water, &x = b.water;
    const sim::RainFrame &r = a.rain, &s = b.rain;
    return a.number == b.number && a.time == b.time && a.stepMs == b.stepMs && a.domain == b.domain &&
           a.fields == b.fields && a.gasTiles == b.gasTiles && w.domain == x.domain && w.band == x.band && w.cells == x.cells &&
           w.tiles == x.tiles && w.flowTiles == x.flowTiles && w.deepTiles == x.deepTiles &&
           w.particles == x.particles && w.litres == x.litres && w.velocities == x.velocities &&
           w.whiteness == x.whiteness && w.positions.size() == x.positions.size() &&
           std::equal(w.positions.begin(), w.positions.end(), x.positions.begin(),
                      [](const Vec3& p, const Vec3& q) { return p.x == q.x && p.y == q.y && p.z == q.z; }) &&
           r.drops == s.drops && r.droplets == s.droplets && r.timeStep == s.timeStep &&
           r.rippleOrigin.x == s.rippleOrigin.x && r.rippleOrigin.y == s.rippleOrigin.y &&
           r.rippleOrigin.z == s.rippleOrigin.z && r.rippleCell == s.rippleCell &&
           r.rippleCells[0] == s.rippleCells[0] && r.rippleCells[1] == s.rippleCells[1] && r.ripples == s.ripples &&
           a.rigid.attribute == b.rigid.attribute && a.rigid.joints == b.rigid.joints &&
           a.rigid.broken == b.rigid.broken && a.rigid.poses == b.rigid.poses && a.rigid.debris == b.rigid.debris &&
           a.rigid.vanished == b.rigid.vanished && w.ids == x.ids && r.dropIds == s.dropIds &&
           r.dropletIds == s.dropletIds && a.rigid.debrisIds == b.rigid.debrisIds &&
           a.rigid.debrisVelocity == b.rigid.debrisVelocity && w.flow == x.flow && a.rigid.rebarState == b.rigid.rebarState &&
           a.rigid.debrisGlass == b.rigid.debrisGlass && a.rigid.unglued == b.rigid.unglued &&
           a.rigid.jointState == b.rigid.jointState && a.rigid.jointTime == b.rigid.jointTime &&
           a.rigid.debrisOrient == b.rigid.debrisOrient && a.grains.positions == b.grains.positions &&
           a.grains.velocities == b.grains.velocities && a.grains.radii == b.grains.radii &&
           a.grains.ids == b.grains.ids && a.grains.colors == b.grains.colors && a.rigid.shatters == b.rigid.shatters;
}

/// What version 15 adds at the end of a frame without grains: five counts, 0.
constexpr size_t kNoGrains = 5 * 8;
/// ... and version 16 after them, when nothing broke as it ran: a count, 0.
constexpr size_t kNoShatters = 8;

/// A frame of every part, made up: runs of zeros of every length in the gas.
sim::Frame madeUpFrame() {
    sim::Frame f;
    f.number = 7;
    f.time = 0.2333f;
    f.stepMs = 12.5;
    f.domain.cells[0] = 16;
    f.domain.cells[1] = 8;
    f.domain.cells[2] = 8;
    f.domain.voxel = 0.05f;
    f.fields.assign(3 * f.domain.cellCount(), 0);
    // Values alone, and runs of 1 to 20 zeros between them; zeros at the end.
    size_t i = 3, gap = 1;
    while (i < f.fields.size() - 40) {
        f.fields[i] = static_cast<uint16_t>(0x3c00 + i);  // 1.0 and on
        i += gap + 1;
        gap = gap % 20 + 1;
    }
    for (size_t k = 100; k < 180; ++k) f.fields[k] = static_cast<uint16_t>(k);  // a run of values
    f.water.domain.cells[0] = f.water.domain.cells[1] = f.water.domain.cells[2] = 8;
    f.water.domain.voxel = 0.1f;
    f.water.band = 0.2f;
    f.water.cells.resize(2 * f.water.domain.cellCount());
    for (size_t k = 0; k < f.water.cells.size(); ++k) f.water.cells[k] = static_cast<uint8_t>(k * 7);
    f.water.particles = 3;
    f.water.litres = 1.5;
    f.water.positions = {{0.1f, 0.2f, 0.3f}, {-0.1f, 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f}};
    f.water.velocities = {0, 0x3c00, 0xbc00, 1, 2, 3, 0, 0, 0};
    f.water.whiteness = {0, 128, 255};
    f.water.ids = {7, 3, 4000000000u};
    // The flow on the solver's grid, 4 x 4 x 4: still water, and a stream.
    f.water.flow.assign(3 * 64, 0);
    for (size_t k = 90; k < 150; ++k) f.water.flow[k] = static_cast<uint16_t>(0x3800 + k);
    f.rain.drops = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    f.rain.droplets = {0.5f, 1, 1.5f, 2, 2.5f, 3};
    f.rain.dropIds = {12, 13};
    f.rain.dropletIds = {99};
    f.rain.timeStep = 1.0f / 30.0f;
    f.rigid.attribute = "piece";
    f.rigid.joints = 5;
    f.rigid.broken = 2;
    for (int k = 0; k < 3; ++k) {
        sim::RigidPose p;
        p.position = Vec3(0.1f * static_cast<float>(k), 1.0f, -0.5f);
        p.rotation = Vec4(0.0f, 0.7071068f, 0.0f, 0.7071068f);
        p.velocity = Vec3(0.0f, -2.0f, 0.5f);
        p.spin = Vec3(1.0f, 0.0f, 0.0f);
        f.rigid.poses.push_back(p);
    }
    f.rigid.debris = {0.0f, 0.1f, 0.2f, 0.05f, 1.0f, 0.0f, 1.0f, 0.02f};  // two bits of grit
    f.rigid.debrisVelocity = {0.0f, -1.0f, 0.0f, 0.5f, 0.0f, 0.0f};
    f.rigid.debrisIds = {40, 41};
    f.rigid.debrisGlass = {0, 1};  // the second a chip of glass
    f.rigid.debrisOrient = {0.0f, 0.0f, 0.0f, 1.0f, 0.5f, -0.5f, 0.5f, 0.5f};  // as it lands, and turned
    f.rigid.unglued = {0, 2};
    f.rigid.vanished = {1};
    f.rigid.rebarState = {0, 1, 2, 0};  // a bar out of one piece, torn after the next
    f.rigid.jointState = {0, 1, 2, 1, 0};  // two of five joints broken, one never held
    f.rigid.jointTime = {0.0f, 0.1f, 0.0f, 0.2333f, 0.0f};
    f.rain.rippleOrigin = {-1.0f, 0.2f, -1.0f};
    f.rain.rippleCell = 0.05f;
    f.rain.rippleCells[0] = 4;
    f.rain.rippleCells[1] = 3;
    f.rain.ripples = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x3555};
    return f;
}

}  // namespace

// --- PLY ------------------------------------------------------------------------------------

TEST(ply_round_trip_keeps_points_attributes_and_faces) {
    const Geometry geo = everything();
    const std::string bytes = io::formatPly(geo);
    const std::string header = bytes.substr(0, bytes.find("end_header\n"));
    CHECK(header.find("format binary_little_endian 1.0") != std::string::npos);
    CHECK(header.find("element vertex 4\n") != std::string::npos);
    CHECK(header.find("property uchar red\n") != std::string::npos);
    CHECK(header.find("property float nx\n") != std::string::npos);
    CHECK(header.find("property float vx\n") != std::string::npos);
    CHECK(header.find("property int id\n") != std::string::npos);
    CHECK(header.find("property float uv_y\n") != std::string::npos);
    CHECK(header.find("property float orient_w\n") != std::string::npos);
    CHECK(header.find("name") == std::string::npos);  // strings stay out
    CHECK(header.find("element face 2\n") != std::string::npos);

    Geometry back;
    std::string error;
    CHECK(io::parsePly(bytes, back, error));
    CHECK(error.empty());
    CHECK_EQ(back.pointCount(), size_t(4));
    CHECK_EQ(back.primitiveCount(), size_t(2));
    for (size_t i = 0; i < 4; ++i) {
        CHECK(near(back.positions()[i], geo.positions()[i]));
        CHECK(near(back.points().find("N")->read<Vec3>()[i], geo.points().find("N")->read<Vec3>()[i]));
        CHECK(near(back.points().find("Cd")->read<Vec3>()[i], geo.points().find("Cd")->read<Vec3>()[i], 1e-6f));
        CHECK(near(back.points().find("v")->read<Vec3>()[i], geo.points().find("v")->read<Vec3>()[i]));
        CHECK(near(back.points().find("pscale")->read<float>()[i], geo.points().find("pscale")->read<float>()[i]));
        CHECK_EQ(back.points().find("id")->read<int32_t>()[i], 16777217 + static_cast<int32_t>(i));
        const Vec2 uv = back.points().find("uv")->read<Vec2>()[i];
        CHECK(near(uv.x, static_cast<float>(i) * 0.25f) && near(uv.y, 1.0f - static_cast<float>(i) * 0.25f));
        const Vec4 q = back.points().find("orient")->read<Vec4>()[i];
        CHECK(q.x == 0.0f && q.y == 0.0f && q.z == static_cast<float>(i) && q.w == 1.0f);
    }
    CHECK_EQ(back.points().find("id")->type(), AttrType::Int);
    CHECK_EQ(back.points().find("uv")->type(), AttrType::Vec2);
    CHECK(back.points().find("name") == nullptr);
    const auto tri = back.primitivePoints(0);
    const auto quad = back.primitivePoints(1);
    CHECK(tri.size() == 3 && tri[0] == 0 && tri[1] == 1 && tri[2] == 2);
    CHECK(quad.size() == 4 && quad[3] == 3);
    CHECK(back.primitiveClosed(1));

    // The same geometry, the same bytes.
    CHECK(io::formatPly(everything()) == bytes);
}

TEST(ply_reads_ascii_with_colour_bytes_alpha_and_faces) {
    const std::string text =
        "ply\r\nformat ascii 1.0\r\ncomment from somewhere else\r\n"
        "element vertex 3\r\nproperty float x\r\nproperty float y\r\nproperty float z\r\n"
        "property uchar red\r\nproperty uchar green\r\nproperty uchar blue\r\nproperty uchar alpha\r\n"
        "property double temperature\r\nproperty short label\r\n"
        "element face 1\r\nproperty list uchar uint vertex_indices\r\nproperty float quality\r\n"
        "end_header\r\n"
        "0 0 0 255 0 0 255 1.5 -3\r\n"
        "1 0 0 0 255 0 128 2.5 4\r\n"
        "0 1 0 0 0 255 0 -0.25 5\r\n"
        "3 0 1 2 0.5\r\n";
    Geometry geo;
    std::string error;
    CHECK(io::parsePly(text, geo, error));
    CHECK_EQ(geo.pointCount(), size_t(3));
    CHECK_EQ(geo.primitiveCount(), size_t(1));
    CHECK(near(geo.positions()[1], Vec3(1.0f, 0.0f, 0.0f)));
    const AttributeArray* cd = geo.points().find("Cd");
    CHECK(cd && cd->type() == AttrType::Vec4);
    const Vec4 c1 = cd->read<Vec4>()[1];
    CHECK(near(c1.x, 0.0f) && near(c1.y, 1.0f) && near(c1.w, 128.0f / 255.0f));
    CHECK(near(geo.points().find("temperature")->read<float>()[2], -0.25f));
    CHECK_EQ(geo.points().find("label")->type(), AttrType::Int);
    CHECK_EQ(geo.points().find("label")->read<int32_t>()[0], -3);
    CHECK(geo.points().find("alpha") == nullptr);  // in Cd

    // Colours as numbers are taken as they are.
    const std::string floats =
        "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\n"
        "property float red\nproperty float green\nproperty float blue\nend_header\n0 0 0 0.5 0.25 1\n";
    CHECK(io::parsePly(floats, geo, error));
    CHECK(near(geo.points().find("Cd")->read<Vec3>()[0], Vec3(0.5f, 0.25f, 1.0f)));
}

TEST(ply_refuses_what_it_cannot_read) {
    Geometry geo;
    std::string error;
    CHECK(!io::parsePly("solid cube\n", geo, error));
    CHECK(error.find("not a PLY") != std::string::npos);
    CHECK(!io::parsePly("ply\nformat binary_big_endian 1.0\nelement vertex 1\nproperty float x\nend_header\n", geo, error));
    CHECK(error.find("binary_big_endian") != std::string::npos);
    CHECK(!io::parsePly("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\n", geo, error));
    CHECK(error.find("end_header") != std::string::npos);
    // A header that promises more than the file holds is not believed --
    // nor is room made for what it promises.
    CHECK(!io::parsePly("ply\nformat binary_little_endian 1.0\nelement vertex 4000000000000\nproperty float x\n"
                        "end_header\n\x01\x02\x03\x04",
                        geo, error));
    CHECK(error.find("fewer") != std::string::npos);
    // Cut short.
    const std::string full = io::formatPly(everything());
    for (size_t cut : {full.find("end_header\n") + 11, full.size() - 1, full.size() - 17}) {
        CHECK(!io::parsePly(full.substr(0, cut), geo, error));
    }
    // A face of vertices that are not there, and one of a vertex below 0.
    const std::string head = "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
                             "element face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n";
    CHECK(!io::parsePly(head + "3 0 1 7\n", geo, error));
    CHECK(error.find("not there") != std::string::npos);
    CHECK(!io::parsePly(head + "3 0 -1 2\n", geo, error));
    CHECK(io::parsePly(head + "3 0 1 2\n", geo, error));
    CHECK_EQ(geo.primitiveCount(), size_t(1));
}

// --- OpenVDB --------------------------------------------------------------------------------

TEST(vdb_files_are_framed_as_openvdb_reads_them) {
    std::vector<float> values(20 * 10 * 9, 0.0f);
    values[0] = 0.5f;                  // voxel (0, 0, 0)
    values[19 + 20 * (9 + 10 * 8)] = 2.0f;  // the far corner
    values[5 + 20 * (3 + 10 * 4)] = 1.25f;
    std::vector<Volume> volumes;
    volumes.push_back(Volume::make("density", Vec3(-1.0f, 0.0f, -0.5f), 0.1f, 20, 10, 9, values));
    std::vector<float> signedValues(values);
    signedValues[7] = -1.0f;
    volumes.push_back(Volume::make("density", Vec3(-1.0f, 0.0f, -0.5f), 0.1f, 20, 10, 9, signedValues));
    volumes.push_back(Volume::make("empty", Vec3(0.0f), 0.1f, 4, 4, 4));

    const std::string bytes = io::formatVdb(volumes);
    VdbFile file(bytes);
    CHECK(file.ok);
    CHECK_EQ(file.version, 224u);
    CHECK_EQ(file.uuid.size(), size_t(36));
    CHECK_EQ(file.uuid[14], '4');  // a version 4 UUID
    CHECK_EQ(file.fileMeta["creator"], std::string("prototype"));
    CHECK_EQ(file.grids.size(), size_t(3));
    CHECK_EQ(file.grids[0].name, std::string("density"));
    CHECK_EQ(file.grids[1].name, std::string("density_2"));  // one name, twice
    CHECK_EQ(file.grids[2].name, std::string("empty"));
    for (const auto& g : file.grids) CHECK_EQ(g.type, std::string("Tree_float_5_4_3"));
    CHECK_EQ(file.grids[0].meta["class"], std::string("fog volume"));
    CHECK_EQ(file.grids[1].meta["class"], std::string("unknown"));  // a value below 0
    CHECK_EQ(int64Of(file.grids[0].meta["file_voxel_count"]), int64_t(3));
    CHECK_EQ(int64Of(file.grids[1].meta["file_voxel_count"]), int64_t(4));
    CHECK_EQ(int64Of(file.grids[2].meta["file_voxel_count"]), int64_t(0));
    // The bounding box of the active voxels, index space.
    const std::string& hi = file.grids[0].meta["file_bbox_max"];
    CHECK_EQ(hi.size(), size_t(12));
    int32_t corner[3];
    std::memcpy(corner, hi.data(), sizeof corner);  // little-endian, as the machines this runs on
    CHECK(corner[0] == 19 && corner[1] == 9 && corner[2] == 8);

    // The same volumes, the same bytes; another value, another file.
    CHECK(io::formatVdb(volumes) == bytes);
    values[1] = 3.0f;
    std::vector<Volume> other = volumes;
    other[0] = Volume::make("density", Vec3(-1.0f, 0.0f, -0.5f), 0.1f, 20, 10, 9, values);
    const std::string changed = io::formatVdb(other);
    CHECK(changed != bytes);
    CHECK(VdbFile(changed).uuid != file.uuid);

    std::string error;
    CHECK(!io::writeVdb({}, "never.vdb", error));
    CHECK(error.find("no volume") != std::string::npos);
}

// --- writing by extension, sequences ----------------------------------------------------------

TEST(frame_paths_number_the_files_of_a_sequence) {
    CHECK_EQ(io::framePath("out/smoke.$F4.vdb", 7), std::string("out/smoke.0007.vdb"));
    CHECK_EQ(io::framePath("out/smoke.$F.vdb", 7), std::string("out/smoke.7.vdb"));
    CHECK_EQ(io::framePath("x.$F4.$F.ply", 12), std::string("x.0012.12.ply"));
    CHECK_EQ(io::framePath("points.ply", 30), std::string("points.0030.ply"));
    CHECK_EQ(io::framePath("dir/points.ply", 12345), std::string("dir/points.12345.ply"));
}

TEST(geometry_is_written_as_its_extension_says) {
    TempFolder dir("export");
    Geometry geo = everything();
    std::string error;
    CHECK(io::writeGeometry(geo, dir / "a.ply", error));
    CHECK(fileText(dir / "a.ply").rfind("ply\n", 0) == 0);
    CHECK(io::writeGeometry(geo, dir / "B.PLY", error));  // case does not matter
    CHECK(io::writeGeometry(geo, dir / "a.obj", error));
    CHECK(fileText(dir / "a.obj").find("\nf ") != std::string::npos);

    // Volumes to .vdb; none there, no file.
    CHECK(!io::writeGeometry(geo, dir / "a.vdb", error));
    CHECK(error.find("no volume") != std::string::npos);
    CHECK(!fs::exists(dir / "a.vdb"));
    geo.addVolume(Volume::make("density", Vec3(0.0f), 0.1f, 8, 8, 8, std::vector<float>(512, 0.25f)));
    CHECK(io::writeGeometry(geo, dir / "a.vdb", error));
    const std::string vdb = fileText(dir / "a.vdb");
    CHECK(VdbFile(vdb).ok);
    CHECK_EQ(int64Of(VdbFile(vdb).grids[0].meta["file_voxel_count"]), int64_t(512));

    CHECK(!io::writeGeometry(geo, dir / "a.abc", error));
    CHECK(error.find(".ply") != std::string::npos && error.find(".vdb") != std::string::npos);
    CHECK(!io::writeGeometry(geo, dir / "no/such/folder/a.ply", error));
    CHECK(error.find("cannot write") != std::string::npos);
}

// --- the cache of a simulation ----------------------------------------------------------------

TEST(frames_round_trip_through_their_files) {
    const sim::Frame f = madeUpFrame();
    const std::string bytes = sim::formatFrame(f);
    CHECK(bytes.rfind(std::string("PGFRAME\0", 8), 0) == 0);
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));

    // Zeros take little room: a gas grid of nothing is a few bytes.
    sim::Frame empty;
    empty.domain.cells[0] = empty.domain.cells[1] = empty.domain.cells[2] = 64;
    empty.fields.assign(3 * empty.domain.cellCount(), 0);
    const std::string small = sim::formatFrame(empty);
    CHECK(small.size() < 448);
    CHECK(sim::parseFrame(small, back, error));
    CHECK(sameFrame(empty, back));

    // Every run of zeros, at the start, between values, at the end.
    for (size_t zeros = 0; zeros < 20; ++zeros) {
        for (size_t values = 0; values < 12; ++values) {
            sim::Frame g;
            g.domain.cells[0] = g.domain.cells[1] = g.domain.cells[2] = 8;
            g.fields.assign(3 * g.domain.cellCount(), 0);
            for (size_t k = 0; k < values; ++k) g.fields[zeros + k] = static_cast<uint16_t>(k + 1);
            for (size_t k = 0; k < values; ++k) g.fields[g.fields.size() - 1 - zeros - k] = static_cast<uint16_t>(k + 100);
            CHECK(sim::parseFrame(sim::formatFrame(g), back, error));
            CHECK(back.fields == g.fields);
        }
    }
}

TEST(frames_of_simulations_round_trip) {
    // Gas, water with its particles, rain: what the solvers make.
    for (const char* example : {"campfire", "liquid_points", "rain_pond"}) {
        sim::Network net;
        CHECK(sim::Network::example(example, net));
        sim::Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
        CHECK(c.ok);
        c.world.gas.solver.resolution = 24;
        c.world.water.solver.resolution = 16;
        sim::WorldSolver solver(c.world);
        for (int i = 0; i < 3; ++i) solver.step();
        const sim::Frame f = solver.capture();
        CHECK(!f.empty());
        sim::Frame back;
        std::string error;
        CHECK(sim::parseFrame(sim::formatFrame(f), back, error));
        CHECK(sameFrame(f, back));
    }
}

TEST(frames_of_version_3_still_read_without_the_particles_numbers) {
    // A frame as version 3 wrote it: the same bytes without what version 4
    // adds at the end -- the particles' numbers and the grit's velocity, here
    // five empty arrays.
    sim::Frame f = madeUpFrame();
    f.water.ids.clear();
    f.rain.dropIds.clear();
    f.rain.dropletIds.clear();
    f.rigid.debrisIds.clear();
    f.rigid.debrisVelocity.clear();
    f.water.flow.clear();
    f.rigid.rebarState.clear();
    f.rigid.debrisGlass.clear();
    f.rigid.unglued.clear();
    f.rigid.jointState.clear();
    f.rigid.jointTime.clear();
    f.rigid.debrisOrient.clear();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters - kNoGrains - 13 * 8);  // the five counts, version 5's, 6's, 7's two, 8's two, 9's and 10's, all 0
    bytes[8] = 3;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    // Numbers that are not one a particle are refused.
    sim::Frame wrong = madeUpFrame();
    wrong.water.ids.pop_back();
    CHECK(!sim::parseFrame(sim::formatFrame(wrong), back, error));
    CHECK(error.find("do not fit") != std::string::npos);
}

TEST(frames_of_version_4_still_read_without_the_waters_flow) {
    // As version 4 wrote it: without the flow at the end.
    sim::Frame f = madeUpFrame();
    f.water.flow.clear();
    f.rigid.rebarState.clear();
    f.rigid.debrisGlass.clear();
    f.rigid.unglued.clear();
    f.rigid.jointState.clear();
    f.rigid.jointTime.clear();
    f.rigid.debrisOrient.clear();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters - kNoGrains - 8 * 8);  // its count, 0, and version 6's, 7's two, 8's two, 9's and 10's
    bytes[8] = 4;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK(back.water.flow.empty());
    CHECK(back.water.flowAt(Vec3(0.1f, 0.2f, 0.0f)) == Vec3());
    // A flow not of the solver's grid is refused.
    sim::Frame wrong = madeUpFrame();
    wrong.water.flow.resize(3 * 63);
    CHECK(!sim::parseFrame(sim::formatFrame(wrong), back, error));
}

TEST(frames_of_version_5_still_read_without_what_became_of_the_bars) {
    // As version 5 wrote it: without the bars' state at the end -- all of
    // them as built, as a frame without bars has them.
    sim::Frame f = madeUpFrame();
    f.rigid.rebarState.clear();
    f.rigid.debrisGlass.clear();
    f.rigid.unglued.clear();
    f.rigid.jointState.clear();
    f.rigid.jointTime.clear();
    f.rigid.debrisOrient.clear();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters - kNoGrains - 7 * 8);  // its count, 0, and version 7's and 8's two, 9's and 10's
    bytes[8] = 5;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK(back.rigid.rebarState.empty());
}

TEST(frames_of_version_6_still_read_without_which_grit_is_glass) {
    // As version 6 wrote it: without the glass and the bodies come loose at
    // the end -- none of the grit glass, none loose, as a frame without glass
    // or glue has it.
    sim::Frame f = madeUpFrame();
    f.rigid.debrisGlass.clear();
    f.rigid.unglued.clear();
    f.rigid.jointState.clear();
    f.rigid.jointTime.clear();
    f.rigid.debrisOrient.clear();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters - kNoGrains - 6 * 8);  // their counts, 0, and version 8's two, 9's and 10's
    bytes[8] = 6;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK(back.rigid.debrisGlass.empty());
    CHECK(back.rigid.unglued.empty());
    // Glass that is not one a bit of grit is refused.
    sim::Frame wrong = madeUpFrame();
    wrong.rigid.debrisGlass.push_back(1);
    CHECK(!sim::parseFrame(sim::formatFrame(wrong), back, error));
}

TEST(frames_of_version_7_still_read_without_what_became_of_the_joints) {
    // As version 7 wrote it: without what became of each joint of the glue
    // at the end -- not known, as a frame without glue has it.
    sim::Frame f = madeUpFrame();
    f.rigid.jointState.clear();
    f.rigid.jointTime.clear();
    f.rigid.debrisOrient.clear();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters - kNoGrains - 4 * 8);  // their counts, 0, and version 9's and 10's
    bytes[8] = 7;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK(back.rigid.jointState.empty());
    // A time for each joint, or refused.
    sim::Frame wrong = madeUpFrame();
    wrong.rigid.jointTime.pop_back();
    CHECK(!sim::parseFrame(sim::formatFrame(wrong), back, error));
}

TEST(frames_of_version_8_still_read_without_how_the_grit_is_turned) {
    // As version 8 wrote it: without the grit's turns at the end -- not
    // known, as a frame without grit has them.
    sim::Frame f = madeUpFrame();
    f.rigid.debrisOrient.clear();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters - kNoGrains - 2 * 8);  // its count, 0, and version 10's
    bytes[8] = 8;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK(back.rigid.debrisOrient.empty());
    // A turn for each bit, or refused.
    sim::Frame wrong = madeUpFrame();
    wrong.rigid.debrisOrient.resize(4);
    CHECK(!sim::parseFrame(sim::formatFrame(wrong), back, error));
}

TEST(frames_of_version_9_still_read_without_sparse_gas) {
    // As version 9 wrote it: without the gas's tiles at the end -- every
    // cell of the grid, as a dense solver's frame still has it.
    const sim::Frame f = madeUpFrame();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters - kNoGrains - 8);  // version 10's count, 0
    bytes[8] = 9;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK(back.gasTiles.empty());
}

TEST(sparse_gas_frames_keep_their_tiles_alone) {
    // A 16 x 8 x 24 grid is 2 x 1 x 3 tiles; two of them hold gas.
    sim::Frame f;
    f.domain.cells[0] = 16;
    f.domain.cells[1] = 8;
    f.domain.cells[2] = 24;
    f.gasTiles = {1, 5};
    f.fields.assign(3 * 512 * 2, 0);
    f.fields[3 * 7 + 1] = sim::toHalf(2.0f);          // tile 1, cell (7, 0, 0): x 15
    f.fields[3 * (512 + 9) + 0] = sim::toHalf(0.5f);  // tile 5, cell (1, 1, 0): x 9, y 1, z 16
    CHECK_EQ(f.at(1, 15, 0, 0), 2.0f);
    CHECK_EQ(f.at(0, 9, 1, 16), 0.5f);
    CHECK_EQ(f.at(0, 0, 0, 0), 0.0f);  // a tile without gas
    std::vector<uint16_t> scratch;
    const std::vector<uint16_t>& dense = f.denseFields(scratch);
    CHECK_EQ(dense.size(), 3 * f.domain.cellCount());
    const size_t cell = 9 + 16 * (1 + 8 * 16);
    CHECK_EQ(sim::fromHalf(dense[3 * cell]), 0.5f);
    size_t some = 0;
    for (const uint16_t v : dense) some += v != 0;
    CHECK_EQ(some, 2u);
    // Coarser grids: the means of the cells under each -- the same from the
    // tiles as from every cell.
    std::vector<uint16_t> half;
    f.coarseFields(2, half);
    CHECK_EQ(half.size(), 3 * size_t(8 * 4 * 12));
    CHECK_EQ(sim::fromHalf(half[3 * 7 + 1]), 0.25f);                    // x 15 of 14..15
    CHECK_EQ(sim::fromHalf(half[3 * (4 + 8 * (0 + 4 * 8))]), 0.0625f);  // x 9, y 1, z 16
    CHECK_EQ(sim::fromHalf(half[0]), 0.0f);
    for (const int factor : {2, 4, 8}) {
        sim::Frame all = f;
        all.gasTiles.clear();
        all.fields = dense;
        std::vector<uint16_t> a, b;
        f.coarseFields(factor, a);
        all.coarseFields(factor, b);
        CHECK(a == b);
        float total = 0.0f;
        for (size_t c = 0; c < a.size(); c += 3) total += sim::fromHalf(a[c]);
        CHECK_EQ(total * static_cast<float>(factor * factor * factor), 0.5f);  // the smoke, all of it
    }
    // Through a file, and refused where the tiles do not fit the grid.
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(sim::formatFrame(f), back, error));
    CHECK(sameFrame(f, back));
    for (const std::vector<uint32_t>& wrong : {std::vector<uint32_t>{5, 1}, std::vector<uint32_t>{1, 6},
                                               std::vector<uint32_t>{1}}) {
        sim::Frame bad = f;
        bad.gasTiles = wrong;
        CHECK(!sim::parseFrame(sim::formatFrame(bad), back, error));
    }
}

TEST(frames_of_version_12_still_read_without_sparse_water) {
    // As version 12 wrote it: without the water's tiles at the end -- every
    // cell of its grids, as a dense solver's frame still has them.
    const sim::Frame f = madeUpFrame();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters - kNoGrains - 3 * 8);  // version 13's two counts and 14's, 0
    bytes[8] = 12;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK(back.water.tiles.empty() && back.water.flowTiles.empty());
}

TEST(frames_of_version_13_still_read_without_deep_water) {
    // As version 13 wrote it: without the tiles deep in the water at the
    // end -- every tile of it with its cells.
    const sim::Frame f = madeUpFrame();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters - kNoGrains - 8);  // version 14's count, 0
    bytes[8] = 13;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK(back.water.deepTiles.empty());
}

TEST(frames_of_version_14_still_read_without_grains) {
    const sim::Frame f = madeUpFrame();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters - kNoGrains);
    bytes[8] = 14;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK(back.grains.empty());
}

TEST(frames_of_version_15_still_read_without_what_broke) {
    const sim::Frame f = madeUpFrame();
    std::string bytes = sim::formatFrame(f);
    bytes.resize(bytes.size() - kNoShatters);
    bytes[8] = 15;
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK(back.rigid.shatters.empty());
}

TEST(frames_keep_the_pieces_that_broke) {
    // Each break -- which body, where, its seed, how many fragments, when
    // -- back as it was; cut short, refused.
    sim::Frame f = madeUpFrame();
    f.rigid.shatters.push_back({3, Vec3(0.1f, -0.25f, 0.4f), 0xDEADBEEFu, 12, 0.5f});
    f.rigid.shatters.push_back({17, Vec3(-1.0f, 2.0f, 0.0f), 7u, 9, 1.25f});
    const std::string bytes = sim::formatFrame(f);
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(bytes, back, error));
    CHECK(sameFrame(f, back));
    CHECK_EQ(back.rigid.shatters.size(), size_t(2));
    CHECK(back.rigid.shatters[1] == f.rigid.shatters[1]);
    CHECK(!sim::parseFrame(bytes.substr(0, bytes.size() - 5), back, error));
}

TEST(frames_keep_their_grains) {
    // Where each grain is, how fast it goes, how big it is, its number and
    // its colour -- and a frame whose grains do not hold together refused.
    sim::Frame f = madeUpFrame();
    sim::GrainFrame& g = f.grains;
    for (int i = 0; i < 5; ++i) {
        g.positions.push_back(Vec3(0.1f * static_cast<float>(i), 0.02f, -0.3f));
        for (int a = 0; a < 3; ++a) g.velocities.push_back(sim::toHalf(i == 2 ? 0.0f : 0.5f * static_cast<float>(a)));
        g.radii.push_back(sim::toHalf(0.01f + 0.001f * static_cast<float>(i)));
        g.ids.push_back(static_cast<uint32_t>(40 + i));
        g.colors.insert(g.colors.end(), {static_cast<uint8_t>(200), static_cast<uint8_t>(160), static_cast<uint8_t>(90 + i)});
    }
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(sim::formatFrame(f), back, error));
    CHECK(sameFrame(f, back));
    CHECK_EQ(back.grains.size(), size_t(5));
    // Standing still, every one: their speeds, all zeros, come back.
    sim::Frame still = f;
    std::fill(still.grains.velocities.begin(), still.grains.velocities.end(), uint16_t(0));
    CHECK(sim::parseFrame(sim::formatFrame(still), back, error));
    CHECK(sameFrame(still, back));
    // Without colours: the look's.
    sim::Frame plain = f;
    plain.grains.colors.clear();
    CHECK(sim::parseFrame(sim::formatFrame(plain), back, error));
    CHECK(back.grains.colors.empty());
    // A number short: refused.
    sim::Frame broken = f;
    broken.grains.ids.pop_back();
    CHECK(!sim::parseFrame(sim::formatFrame(broken), back, error));
    CHECK(error.find("grains") != std::string::npos);
}

TEST(sparse_water_frames_keep_their_tiles_alone) {
    // A grid of 16 x 8 x 24 cells is 2 x 1 x 3 tiles; two of them near the
    // water. The flow's grid, half as fine -- 8 x 4 x 12 cells, 1 x 1 x 2
    // tiles --, has one.
    sim::Frame f;
    sim::WaterFrame& w = f.water;
    w.domain.cells[0] = 16;
    w.domain.cells[1] = 8;
    w.domain.cells[2] = 24;
    w.domain.voxel = 0.0625f;  // the cells' centres exactly where they are
    w.band = 0.125f;
    w.tiles = {1, 4};
    w.cells.assign(2 * 512 * 2, 0);
    for (size_t c = 0; c < w.cells.size(); c += 2) w.cells[c] = 255;
    w.cells[2 * 7] = 0;                      // tile 1, cell (7, 0, 0): x 15, in the water
    w.cells[2 * (512 + 9) + 1] = 255;        // tile 4, cell (1, 1, 0): x 1, y 1, z 16, white
    w.flowTiles = {1};
    w.flow.assign(3 * 512, 0);
    w.flow[3 * 2] = sim::toHalf(1.5f);       // tile 1, cell (2, 0, 0): x 2, z 8
    w.deepTiles = {2};                       // x 0 to 7, z 8 to 15: deep in the water, without cells
    CHECK(w.fits() && w.hasFlow());
    CHECK_EQ(w.distance(3, 2, 9), -w.band);
    CHECK_EQ(w.foam(3, 2, 9), 0.0f);
    CHECK_EQ(w.distance(15, 0, 0), -w.band);
    CHECK_EQ(w.distance(0, 0, 0), w.band);   // a tile not kept: far from the water
    CHECK_EQ(w.foam(1, 1, 16), 1.0f);
    CHECK_EQ(w.foam(1, 1, 0), 0.0f);
    const sim::Domain d = w.flowDomain();
    const Vec3 centre = d.origin() + Vec3(2.5f * d.voxel, 0.5f * d.voxel, 8.5f * d.voxel);
    CHECK(w.flowAt(centre) == Vec3(1.5f, 0.0f, 0.0f));
    CHECK(w.flowAt(d.origin() + Vec3(0.5f * d.voxel)) == Vec3());  // a tile not kept: still
    // Every cell, and every cell of coarser grids: their means.
    std::vector<uint8_t> scratch;
    const std::vector<uint8_t>& all = w.denseCells(scratch);
    CHECK_EQ(all.size(), 2 * w.domain.cellCount());
    CHECK_EQ(all[2 * 15], 0);
    CHECK_EQ(all[2 * (1 + 16 * (1 + 8 * 16)) + 1], 255);
    CHECK_EQ(all[2 * 3], 255);
    CHECK_EQ(all[2 * (3 + 16 * (2 + 8 * 9))], 0);  // deep
    std::vector<uint8_t> half;
    w.coarseCells(2, half);
    CHECK_EQ(half.size(), 2 * size_t(8 * 4 * 12));
    CHECK_EQ(half[2 * 7], (7 * 255 + 4) / 8);         // one of the eight in the water
    CHECK_EQ(half[2 * (0 + 8 * (0 + 4 * 8)) + 1], (255 + 4) / 8);  // one of the eight white
    CHECK_EQ(half[2 * 1], 255);
    CHECK_EQ(half[2 * (1 + 8 * (1 + 4 * 4))], 0);  // deep
    for (const int factor : {2, 4, 8}) {
        // As from every cell.
        sim::WaterFrame dense = w;
        dense.tiles.clear();
        dense.cells = all;
        std::vector<uint8_t> a, b;
        w.coarseCells(factor, a);
        dense.coarseCells(factor, b);
        CHECK(a == b);
    }
    std::vector<uint16_t> flowScratch;
    const std::vector<uint16_t>& flow = w.denseFlow(flowScratch);
    CHECK_EQ(flow.size(), 3 * d.cellCount());
    CHECK_EQ(sim::fromHalf(flow[3 * (2 + 8 * 4 * 8)]), 1.5f);
    // Through a file, and refused where the tiles do not fit the grids.
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(sim::formatFrame(f), back, error));
    CHECK(sameFrame(f, back));
    for (const std::vector<uint32_t>& wrong : {std::vector<uint32_t>{4, 1}, std::vector<uint32_t>{1, 6},
                                               std::vector<uint32_t>{1}}) {
        sim::Frame bad = f;
        bad.water.tiles = wrong;
        CHECK(!sim::parseFrame(sim::formatFrame(bad), back, error));
    }
    for (const std::vector<uint32_t>& wrong : {std::vector<uint32_t>{2}, std::vector<uint32_t>{0, 1}}) {
        sim::Frame bad = f;
        bad.water.flowTiles = wrong;
        CHECK(!sim::parseFrame(sim::formatFrame(bad), back, error));
    }
    // Deep tiles that are kept with cells too, out of order, beyond the grid.
    for (const std::vector<uint32_t>& wrong : {std::vector<uint32_t>{1}, std::vector<uint32_t>{5, 2},
                                               std::vector<uint32_t>{2, 6}}) {
        sim::Frame bad = f;
        bad.water.deepTiles = wrong;
        CHECK(!sim::parseFrame(sim::formatFrame(bad), back, error));
    }
}

TEST(frames_that_are_not_what_they_say_are_refused) {
    const std::string bytes = sim::formatFrame(madeUpFrame());
    sim::Frame f;
    std::string error;
    CHECK(!sim::parseFrame("PGFRAMX", f, error));
    CHECK(error.find("not a frame") != std::string::npos);
    // Cut anywhere.
    for (size_t cut = 0; cut < bytes.size(); cut += 37) CHECK(!sim::parseFrame(bytes.substr(0, cut), f, error));
    // A newer version.
    std::string newer = bytes;
    newer[8] = 17;
    CHECK(!sim::parseFrame(newer, f, error));
    CHECK(error.find("newer") != std::string::npos);
    // A grid larger than any solver's, and a gas that does not fill its grid.
    std::string huge = bytes;
    const int32_t cells = 100000;
    std::memcpy(&huge[8 + 4 + 4 + 4 + 8], &cells, 4);  // magic, version, number, time, step: the gas's x cells
    CHECK(!sim::parseFrame(huge, f, error));
    std::string wrong = bytes;
    const int32_t other = 24;
    std::memcpy(&wrong[8 + 4 + 4 + 4 + 8], &other, 4);
    CHECK(!sim::parseFrame(wrong, f, error));
    // Every byte flipped in turn: refused or read, never a crash.
    for (size_t i = 0; i < bytes.size(); i += 3) {
        std::string flipped = bytes;
        flipped[i] = static_cast<char>(flipped[i] ^ 0x5a);
        sim::parseFrame(flipped, f, error);
    }
}

TEST(a_cache_is_a_folder_of_frames_and_a_note) {
    TempFolder dir("cache");
    const std::string folder = dir / "shot";  // made when the first frame is written
    sim::Frame a = madeUpFrame(), b = madeUpFrame();
    a.number = 1;
    b.number = 2;
    b.fields[3] = 0x4000;
    std::string error;
    CHECK(sim::writeFrame(a, folder, error));
    CHECK(sim::writeFrame(b, folder, error));
    CHECK_EQ(fs::path(sim::frameFile(folder, 2)).filename().string(), std::string("frame.0002.pgframe"));
    CHECK(fs::exists(sim::frameFile(folder, 2)));

    sim::CacheInfo info;
    info.frames = 2;
    info.fps = 1.0f / (1.0f / 30.0f);  // 29.999998
    info.network = 0x0123456789abcdefull;
    CHECK(sim::writeCacheInfo(folder, info, error));
    const std::string note = fileText(folder + "/cache.txt");
    CHECK(note.rfind("pgcache 1\n", 0) == 0);
    CHECK(note.find("fps 30\n") != std::string::npos);
    CHECK(note.find("network 0123456789abcdef\n") != std::string::npos);

    sim::CacheInfo read;
    CHECK(sim::readCacheInfo(folder, read, error));
    CHECK_EQ(read.frames, 2);
    CHECK_EQ(read.fps, 30.0f);
    CHECK_EQ(read.network, info.network);
    sim::Frame back;
    CHECK(sim::readFrame(folder, 2, back, error));
    CHECK(sameFrame(b, back));
    CHECK(!sim::readFrame(folder, 3, back, error));
    CHECK(error.find("frame.0003.pgframe") != std::string::npos);
    CHECK(!sim::readCacheInfo(dir / "nothing", read, error));
    CHECK(error.find("no simulation cache") != std::string::npos);
}

TEST(a_network_hash_leaves_out_where_the_nodes_sit) {
    sim::Network net;
    CHECK(sim::Network::example("campfire", net));
    const std::string text = net.save();
    // The first node moved on the canvas: its line ends with other numbers.
    const size_t line = text.find("\nnode ") + 1;
    const size_t end = text.find('\n', line);
    std::string moved = text;
    const size_t y = moved.rfind(' ', end - 1);
    const size_t x = moved.rfind(' ', y - 1);
    moved.replace(x + 1, end - x - 1, "123.5 -40");
    CHECK(moved != text);
    CHECK_EQ(sim::networkHash(moved), sim::networkHash(text));
    // A parameter changed: another network.
    const int fire = net.named("fire")->id;
    CHECK(net.setParam(fire, "fuel", {20.0f, 0.0f, 0.0f}));
    CHECK(sim::networkHash(net.save()) != sim::networkHash(text));
}
