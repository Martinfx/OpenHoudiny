//
// Alembic: the Ogawa container, the archive layer on it, and files written
// by the library itself (Blender 4.5's exporter, tests/data/abc).
//
#include "pg/abc/Archive.h"
#include "pg/abc/Geom.h"
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/io/Ogawa.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/AbcExport.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/sim/Rigid.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace pg;
namespace ogawa = pg::io::ogawa;
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

/// A box of `size` at `center`, cut into `count` Voronoi pieces.
GeometryPtr fracturedBox(Vec3 center, Vec3 size, int count) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setInt("divisions", 1);
    box->setVec3("size", size);
    box->setVec3("center", center);
    pg::Node* fracture = g.create("voronoifracture", "fracture");
    fracture->setInt("count", count);
    fracture->setInt("seed", 3);
    fracture->setInput(0, box);
    CookEngine engine;
    return engine.cook(*fracture, CookContext{});
}

const std::string kData = PG_TEST_DATA_DIR "/abc/";

std::vector<double> doubles(const abc::Sample& s) {
    std::vector<double> out(s.bytes.size() / 8);
    std::memcpy(out.data(), s.bytes.data(), out.size() * 8);
    return out;
}

std::vector<int32_t> ints(const abc::Sample& s) {
    std::vector<int32_t> out(s.bytes.size() / 4);
    std::memcpy(out.data(), s.bytes.data(), out.size() * 4);
    return out;
}

const abc::ObjectReader* objectAt(const abc::ArchiveReader& a, const std::string& path) {
    for (const abc::ObjectReader* o : a.objects()) {
        if (o->path == path) return o;
    }
    return nullptr;
}

}  // namespace

TEST(alembic_ogawa_groups_and_data_read_back) {
    ogawa::Writer w;
    w.openMemory();
    const ogawa::Entry abc = w.data("abc", 3);
    const ogawa::Entry none = w.data(nullptr, 0);
    CHECK_EQ(none, ogawa::kEmptyData);
    const ogawa::Entry pair[2] = {abc, none};
    const ogawa::Entry g = w.group(pair);
    CHECK_EQ(w.group({}), ogawa::kEmptyGroup);
    std::string error;
    const ogawa::Entry root[3] = {g, ogawa::kEmptyGroup, abc};
    CHECK(w.close(root, error));
    const std::string bytes = w.bytes();
    CHECK_EQ(bytes.substr(0, 8), std::string("Ogawa\xff\x00\x01", 8));

    ogawa::Reader r;
    CHECK(r.openMemory(bytes, error));
    std::vector<ogawa::Entry> children;
    CHECK(r.children(r.root(), children, error));
    CHECK_EQ(children.size(), size_t(3));
    std::vector<ogawa::Entry> inner;
    CHECK(r.children(children[0], inner, error));
    CHECK(inner.size() == 2 && inner[1] == ogawa::kEmptyData);
    std::vector<uint8_t> data;
    CHECK(r.data(inner[0], data, error));
    CHECK(data == std::vector<uint8_t>({'a', 'b', 'c'}));
    CHECK(r.data(inner[0], 1, 2, data, error));
    CHECK(data == std::vector<uint8_t>({'b', 'c'}));
    CHECK(!r.data(inner[0], 2, 2, data, error));  // past its end
    uint64_t size = 1;
    CHECK(r.dataSize(ogawa::kEmptyData, size, error) && size == 0);
    CHECK(r.children(ogawa::kEmptyGroup, inner, error) && inner.empty());
    // A group asked for as data, data as a group: refused.
    CHECK(!r.children(abc, inner, error));
    CHECK(!r.dataSize(children[0], size, error));

    // Not finished, not Ogawa, the root out of the file: refused at open.
    std::string unfinished = bytes;
    unfinished[5] = 0;
    CHECK(!r.openMemory(unfinished, error));
    CHECK(error.find("never finished") != std::string::npos);
    CHECK(!r.openMemory("Ogawb" + bytes.substr(5), error));
    std::string far = bytes;
    far[8] = static_cast<char>(0xf0);
    far[9] = static_cast<char>(0xff);
    CHECK(!r.openMemory(far, error));
}

TEST(alembic_archive_samples_read_back_as_written) {
    abc::ArchiveWriter w;
    w.openMemory({{"_ai_Application", "pgtests"}, {"FramesPerTimeUnit", "24"}});
    const uint32_t ts = w.timeSampling(abc::TimeSampling::uniform(1.0 / 24.0, 1.0 / 24.0));
    CHECK_EQ(ts, 1u);
    CHECK_EQ(w.timeSampling(abc::TimeSampling::uniform(1.0 / 24.0, 1.0 / 24.0)), 1u);  // the same one
    abc::ObjectWriter& thing = w.top().child("thing", {{"schema", "Test_v1"}});
    abc::CompoundWriter& geom = thing.properties().compound(".geom", {{"schema", "Test_v1"}});
    abc::PropertyWriter& pos = geom.scalar("pos", abc::Pod::F64, 3, ts);
    abc::PropertyWriter& ids = geom.array("ids", abc::Pod::I32, 1, ts, {{"geoScope", "var"}});
    abc::PropertyWriter& flag = geom.scalar("flag", abc::Pod::Bool, 1, ts);
    geom.compound("empty");
    const double a[3] = {1, 2, 3}, b[3] = {4, 5, 6}, c[3] = {7, 8, 9};
    const double* positions[6] = {a, a, b, b, c, c};
    const std::vector<std::vector<int32_t>> lists = {{1, 2, 3}, {4}, {}, {4}, {4}, {5, 6}};
    const uint8_t on = 1;
    for (int f = 0; f < 6; ++f) {
        pos.scalar(positions[f]);
        ids.array(lists[static_cast<size_t>(f)].data(), lists[static_cast<size_t>(f)].size());
        flag.scalar(&on);
    }
    thing.child("leaf");
    std::string error;
    CHECK(w.close(error));

    abc::ArchiveReader r;
    CHECK(r.openMemory(w.bytes(), error));
    CHECK_EQ(r.meta().at("_ai_Application"), std::string("pgtests"));
    CHECK_EQ(r.libraryVersion(), 10803);
    CHECK_EQ(r.timeSamplings().size(), size_t(2));
    CHECK_EQ(r.timeSamplings()[1].maxSamples, 6u);
    const abc::ObjectReader* o = objectAt(r, "/thing");
    CHECK(o != nullptr);
    if (!o) return;
    CHECK_EQ(o->meta.at("schema"), std::string("Test_v1"));
    CHECK(objectAt(r, "/thing/leaf") != nullptr);
    const abc::PropertyReader* g = o->properties.find(".geom");
    CHECK(g && g->is(abc::PropertyType::Compound) && g->children.size() == 4);
    if (!g || g->children.size() != 4) return;
    const abc::PropertyReader& p = g->children[0];
    CHECK_EQ(p.header.name, std::string("pos"));
    // A, A, B, B, C, C: it changes at 2 and last at 4; the rest are not stored again.
    CHECK_EQ(p.header.samples, 6u);
    CHECK_EQ(p.header.firstChanged, 2u);
    CHECK_EQ(p.header.lastChanged, 4u);
    abc::Sample s;
    for (size_t i = 0; i < 6; ++i) {
        CHECK(r.read(p, i, s, error));
        const std::vector<double> v = doubles(s);
        CHECK(v.size() == 3 && v[0] == positions[i][0] && v[2] == positions[i][2]);
        CHECK(std::fabs(r.timeOf(p, i) - static_cast<double>(i + 1) / 24.0) < 1e-12);
    }
    const abc::PropertyReader& list = g->children[1];
    CHECK(list.is(abc::PropertyType::Array) && list.header.meta.at("geoScope") == "var");
    for (size_t i = 0; i < 6; ++i) {
        CHECK(r.read(list, i, s, error));
        CHECK(ints(s) == lists[i]);
        CHECK_EQ(s.count, lists[i].size());
    }
    const abc::PropertyReader& constant = g->children[2];
    CHECK(constant.header.constant() && constant.header.samples == 6);
    CHECK(r.read(constant, 5, s, error) && s.bytes.size() == 1 && s.bytes[0] == 1);
    CHECK(g->children[3].is(abc::PropertyType::Compound) && g->children[3].children.empty());
    // Past the last sample: the last.
    CHECK(r.read(p, 99, s, error) && doubles(s)[0] == 7.0);
}

TEST(alembic_time_sampling_brackets_a_time) {
    const abc::TimeSampling uniform = abc::TimeSampling::uniform(0.5, 1.0);
    CHECK_EQ(uniform.timeAt(3), 2.5);
    size_t lo = 0, hi = 0;
    double t = 0.0;
    uniform.bracket(1.75, 10, lo, hi, t);
    CHECK(lo == 1 && hi == 2 && std::fabs(t - 0.5) < 1e-12);
    uniform.bracket(0.0, 10, lo, hi, t);
    CHECK(lo == 0 && hi == 0);
    uniform.bracket(99.0, 10, lo, hi, t);
    CHECK(lo == 9 && hi == 9);
    // A frame asked for a hair before its time -- f / fps, rounded otherwise
    // than it was written -- is that frame, not the one before it held.
    const abc::TimeSampling frames = abc::TimeSampling::uniform(1.0 / 30.0, 1.0 / 30.0);
    frames.bracket(3.0 / 30.0 - 1e-12, 10, lo, hi, t);
    CHECK(lo == 2 && hi == 2 && t == 0.0);
    frames.bracket(3.0 / 30.0 + 1e-12, 10, lo, hi, t);
    CHECK(lo == 2 && t == 0.0);
    frames.bracket(10.0 / 30.0 - 1e-12, 10, lo, hi, t);
    CHECK(lo == 9 && hi == 9);
    abc::TimeSampling cyclic;
    cyclic.timePerCycle = 1.0;
    cyclic.times = {0.0, 0.25};
    CHECK_EQ(cyclic.timeAt(3), 1.25);
    abc::TimeSampling acyclic;
    acyclic.timePerCycle = abc::TimeSampling::kAcyclic;
    acyclic.times = {0.0, 0.1, 0.7};
    CHECK(acyclic.acyclic() && acyclic.timeAt(2) == 0.7);
    acyclic.bracket(0.4, 3, lo, hi, t);
    CHECK(lo == 1 && hi == 2 && std::fabs(t - 0.5) < 1e-12);
}

TEST(alembic_reads_what_the_library_writes) {
    // Blender 4.5 wrote it through Alembic 1.8.3 (make_blender_abc.py).
    abc::ArchiveReader r;
    std::string error;
    CHECK(r.open(kData + "shapes.abc", error));
    CHECK_EQ(r.libraryVersion(), 10803);
    CHECK_EQ(r.meta().at("_ai_Application"), std::string("Blender"));
    for (const char* path : {"/box", "/box/Cube", "/spinner", "/spinner/Cube_001", "/cam", "/cam/Camera", "/path"}) {
        CHECK(objectAt(r, path) != nullptr);
    }
    const abc::ObjectReader* spinner = objectAt(r, "/spinner");
    if (!spinner) return;
    CHECK_EQ(spinner->meta.at("schemaObjTitle"), std::string("AbcGeom_Xform_v3:.xform"));
    const abc::PropertyReader* xform = spinner->properties.find(".xform");
    const abc::PropertyReader* vals = xform ? xform->find(".vals") : nullptr;
    CHECK(vals != nullptr);
    if (!vals) return;
    // Still for two frames, then moving: it changes at sample 2 and last at 3.
    CHECK(vals->header.samples == 4 && vals->header.firstChanged == 2 && vals->header.lastChanged == 3);
    CHECK(vals->header.pod == abc::Pod::F64 && vals->header.extent == 16);
    abc::Sample s0, s1, s3;
    CHECK(r.read(*vals, 0, s0, error) && r.read(*vals, 1, s1, error) && r.read(*vals, 3, s3, error));
    CHECK(s0.bytes == s1.bytes);
    const std::vector<double> m0 = doubles(s0), m3 = doubles(s3);
    // A matrix, rows: where it is in the last; moved from x 2 to 3.
    CHECK(m0.size() == 16 && std::fabs(m0[12] - 2.0) < 1e-6 && std::fabs(m3[12] - 3.0) < 1e-6);
    CHECK(std::fabs(r.timeOf(*vals, 0) - 1.0 / 24.0) < 1e-9);
    // The camera's lens: 35 mm on a 36 x 24 mm back (in centimetres).
    const abc::ObjectReader* camera = objectAt(r, "/cam/Camera");
    const abc::PropertyReader* core = camera ? camera->properties.find(".geom") : nullptr;
    core = core ? core->find(".core") : nullptr;
    CHECK(core != nullptr);
    if (!core) return;
    abc::Sample lens;
    CHECK(r.read(*core, 0, lens, error));
    const std::vector<double> c = doubles(lens);
    CHECK(c.size() == 16 && c[0] == 35.0 && std::fabs(c[1] - 3.6) < 1e-9);
}

TEST(alembic_broken_files_are_refused_not_crashed_on) {
    std::ifstream in(kData + "shapes.abc", std::ios::binary);
    const std::string good((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(good.size() > 1000);
    std::mt19937 random(11);
    int refused = 0;
    for (int trial = 0; trial < 300; ++trial) {
        std::string bytes = good;
        const int flips = 1 + static_cast<int>(random() % 8);
        for (int k = 0; k < flips; ++k) bytes[random() % bytes.size()] = static_cast<char>(random());
        if (trial % 7 == 0) bytes.resize(random() % bytes.size());
        abc::ArchiveReader r;
        std::string error;
        if (!r.openMemory(bytes, error)) {
            ++refused;
            continue;
        }
        // Whatever opens, every sample of it reads or says why.
        for (const abc::ObjectReader* o : r.objects()) {
            std::vector<const abc::PropertyReader*> stack = {&o->properties};
            while (!stack.empty()) {
                const abc::PropertyReader* p = stack.back();
                stack.pop_back();
                for (const abc::PropertyReader& c : p->children) stack.push_back(&c);
                if (p->is(abc::PropertyType::Compound)) continue;
                abc::Sample s;
                for (size_t i = 0; i < std::min<size_t>(p->header.samples, 8); ++i) r.read(*p, i, s, error);
            }
        }
    }
    CHECK(refused > 0);
}

namespace {

/// A quad and a triangle (corner normals and uv, point colours and
/// velocities), an open line, and two loose points with ids and sizes.
std::shared_ptr<Geometry> sampleGeometry() {
    auto g = std::make_shared<Geometry>();
    g->addPoints(10);
    auto P = g->positionsForWrite();
    const Vec3 at[10] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}, {3, 0, 0}, {2, 1, 0}, {0, 2, 0}, {1, 2, 1}, {5, 5, 5}};
    for (int i = 0; i < 10; ++i) P[static_cast<size_t>(i)] = at[i];
    const uint32_t quad[4] = {0, 1, 2, 3}, tri[3] = {4, 5, 6}, line[2] = {7, 8};
    g->addPrimitive(quad, true);
    g->addPrimitive(tri, true);
    g->addPrimitive(line, false);
    g->points().create("id", AttrType::Int);  // loose point 9 only matters
    auto ids = g->points().find("id")->write<int32_t>();
    for (size_t i = 0; i < 10; ++i) ids[i] = static_cast<int32_t>(100 + i);
    auto cd = g->points().create("Cd", AttrType::Vec3).write<Vec3>();
    auto v = g->points().create("v", AttrType::Vec3).write<Vec3>();
    auto ps = g->points().create("pscale", AttrType::Float).write<float>();
    for (size_t i = 0; i < 10; ++i) {
        cd[i] = Vec3(0.1f * static_cast<float>(i), 0.5f, 1.0f);
        v[i] = Vec3(0.0f, static_cast<float>(i), 0.0f);
        ps[i] = 0.05f * static_cast<float>(i + 1);
    }
    auto n = g->vertices().create("N", AttrType::Vec3).write<Vec3>();
    auto uv = g->vertices().create("uv", AttrType::Vec3).write<Vec3>();
    for (size_t k = 0; k < g->vertexCount(); ++k) {
        n[k] = Vec3(0.0f, 0.0f, 1.0f);
        uv[k] = Vec3(0.1f * static_cast<float>(k), 0.2f, 0.0f);
    }
    return g;
}

}  // namespace

TEST(alembic_geometry_written_reads_back_in_the_world) {
    const auto geo = sampleGeometry();
    abc::ArchiveWriter w;
    w.openMemory({{"_ai_Application", "pgtests"}});
    const uint32_t ts = w.timeSampling(abc::TimeSampling::uniform(1.0 / 30.0, 1.0 / 30.0));
    abc::XformWriter thing(w.top(), "thing", ts);
    abc::MeshWriter mesh(thing.object(), "mesh", ts);
    abc::CurvesWriter lines(thing.object(), "lines", ts);
    abc::PointsWriter dots(thing.object(), "dots", ts);
    abc::XformWriter rig(w.top(), "rig", ts);
    abc::CameraWriter camera(rig.object(), "camera", ts);
    for (int f = 1; f <= 3; ++f) {
        // Moved up a metre a frame; hidden at frame 3.
        thing.sample(abc::pose(Vec4(0, 0, 0, 1), Vec3(0.0f, static_cast<float>(f), 0.0f)), f < 3);
        mesh.sample(abc::meshSample(*geo));
        lines.sample(abc::curvesSample(*geo));
        dots.sample(abc::pointsSample(*geo));
        // Turned half round about y, standing at z 10.
        rig.sample(abc::pose(Vec4(0, 1, 0, 0), Vec3(0.0f, 1.5f, 10.0f)));
        abc::Lens lens;
        lens.focalLength = 30.0 + f;
        camera.sample(lens);
    }
    thing.finish();
    rig.finish();
    std::string error;
    CHECK(w.close(error));

    abc::ArchiveReader r;
    CHECK(r.openMemory(w.bytes(), error));
    const double time2 = 2.0 / 30.0;
    std::vector<std::string> skipped;
    const auto back = abc::importGeometry(r, time2, abc::ImportOptions{}, &skipped);
    CHECK(skipped.empty());
    // The mesh's 7 points, the line's 2 and the loose one; a quad, a
    // triangle and the line.
    CHECK_EQ(back->pointCount(), size_t(10));
    CHECK_EQ(back->primitiveCount(), size_t(3));
    const auto P = back->positions();
    CHECK(length(P[0] - Vec3(0.0f, 2.0f, 0.0f)) < 1e-6f);  // moved up two metres at frame 2
    // Wound as they were: the quad's corners in its order.
    const auto quad = back->primitivePoints(0);
    CHECK(quad.size() == 4 && length(P[quad[1]] - Vec3(1.0f, 2.0f, 0.0f)) < 1e-6f && length(P[quad[2]] - Vec3(1.0f, 3.0f, 0.0f)) < 1e-6f);
    CHECK(!back->primitiveClosed(2));
    // Their values, where they belong.
    const AttributeArray* N = back->vertices().find("N");
    const AttributeArray* uv = back->vertices().find("uv");
    const AttributeArray* cd = back->points().find("Cd");
    const AttributeArray* v = back->points().find("v");
    CHECK(N && uv && cd && v);
    if (N && uv && cd && v) {
        CHECK(length(N->read<Vec3>()[0] - Vec3(0, 0, 1)) < 1e-6f);
        CHECK(std::fabs(uv->read<Vec3>()[2].x - 0.2f) < 1e-6f);  // the quad's third corner
        CHECK(std::fabs(cd->read<Vec3>()[2].x - 0.2f) < 1e-6f);
        CHECK(std::fabs(v->read<Vec3>()[3].y - 3.0f) < 1e-6f);
    }
    const AttributeArray* id = back->points().find("id");
    const AttributeArray* ps = back->points().find("pscale");
    CHECK(id && ps);
    if (id && ps) {
        CHECK_EQ(id->read<int32_t>()[9], 109);  // the loose point keeps its number
        CHECK(std::fabs(ps->read<float>()[9] - 0.5f) < 1e-6f);
    }
    CHECK_EQ(primitiveString(*back, "path", 0), std::string("/thing/mesh"));
    CHECK_EQ(primitiveString(*back, "path", 2), std::string("/thing/lines"));
    // Hidden at frame 3: nothing then, unless asked.
    CHECK_EQ(abc::importGeometry(r, 3.0 / 30.0, abc::ImportOptions{})->pointCount(), size_t(0));
    abc::ImportOptions all;
    all.hidden = true;
    CHECK_EQ(abc::importGeometry(r, 3.0 / 30.0, all)->pointCount(), size_t(10));
    CHECK(abc::geometryVaries(r, abc::ImportOptions{}));
    // Between frames 1 and 2, half way.
    const auto between = abc::importGeometry(r, 1.5 / 30.0, abc::ImportOptions{});
    CHECK(std::fabs(between->positions()[0].y - 1.5f) < 1e-5f);

    // The camera: where the rig puts it, its lens of the frame.
    const auto cams = abc::cameras(r);
    CHECK_EQ(cams.size(), size_t(1));
    if (cams.empty()) return;
    abc::Matrix world;
    abc::Lens lens;
    CHECK(abc::cameraAt(r, *cams[0], time2, world, lens, error));
    CHECK(std::fabs(lens.focalLength - 32.0) < 1e-9);
    CHECK(std::fabs(world[12]) < 1e-9 && std::fabs(world[13] - 1.5) < 1e-9 && std::fabs(world[14] - 10.0) < 1e-9);
    // Turned half round about y: its own -z looks along the world's +z.
    const Vec3 forward = abc::transformDirection(world, Vec3(0.0f, 0.0f, -1.0f));
    CHECK(length(forward - Vec3(0.0f, 0.0f, 1.0f)) < 1e-6f);
    CHECK(abc::cameraVaries(r, *cams[0]));
}

TEST(alembic_imports_what_blender_writes) {
    std::string error;
    const auto archive = abc::ArchiveReader::openCached(kData + "shapes.abc", error);
    CHECK(archive != nullptr);
    if (!archive) return;
    const abc::ArchiveReader& r = *archive;
    // Frame 1 of 24 a second.
    const auto one = abc::importGeometry(r, 1.0 / 24.0, abc::ImportOptions{});
    // The box, the spinner and the curve's four points.
    CHECK_EQ(one->primitiveCount(), size_t(6 + 6 + 1));
    // Blender's box stood on its floor at z 0.5: Y up, from 0 to 1.
    float lo = 1e9f, hi = -1e9f;
    Vec3 middle(0.0f);
    int boxPoints = 0;
    for (size_t p = 0; p < one->primitiveCount(); ++p) {
        if (primitiveString(*one, "path", p) != "/box/Cube") continue;
        for (const uint32_t q : one->primitivePoints(p)) {
            lo = std::min(lo, one->positions()[q].y);
            hi = std::max(hi, one->positions()[q].y);
            middle += one->positions()[q];
            ++boxPoints;
        }
    }
    CHECK(std::fabs(lo) < 1e-5f && std::fabs(hi - 1.0f) < 1e-5f);
    middle /= static_cast<float>(std::max(boxPoints, 1));
    // Every face of it wound to face out.
    int outward = 0, faces = 0;
    for (size_t p = 0; p < one->primitiveCount(); ++p) {
        if (primitiveString(*one, "path", p) != "/box/Cube") continue;
        const auto q = one->primitivePoints(p);
        const auto P = one->positions();
        const Vec3 n = cross(P[q[1]] - P[q[0]], P[q[2]] - P[q[0]]);
        const Vec3 c = (P[q[0]] + P[q[2]]) * 0.5f;
        outward += dot(n, c - middle) > 0.0f ? 1 : 0;
        ++faces;
    }
    CHECK(faces == 6 && outward == 6);
    // The spinner stays two frames, then moves a metre along x by frame 4.
    auto centre = [&](const Geometry& g) {
        Vec3 c(0.0f);
        int n = 0;
        for (size_t p = 0; p < g.primitiveCount(); ++p) {
            if (primitiveString(g, "path", p) != "/spinner/Cube_001") continue;
            for (const uint32_t q : g.primitivePoints(p)) c += g.positions()[q], ++n;
        }
        return c / static_cast<float>(std::max(n, 1));
    };
    CHECK(length(centre(*one) - Vec3(2.0f, 0.0f, 0.0f)) < 1e-4f);
    CHECK(length(centre(*abc::importGeometry(r, 2.0 / 24.0, abc::ImportOptions{})) - Vec3(2.0f, 0.0f, 0.0f)) < 1e-4f);
    CHECK(length(centre(*abc::importGeometry(r, 4.0 / 24.0, abc::ImportOptions{})) - Vec3(3.0f, 0.0f, 0.0f)) < 1e-4f);
    // Only the spinner, asked for by its path.
    abc::ImportOptions spinner;
    spinner.roots = {"/spinner"};
    CHECK_EQ(abc::importGeometry(r, 1.0 / 24.0, spinner)->primitiveCount(), size_t(6));
    // The camera: Blender's (0, -6, 1.5), Y up; 35 mm.
    const auto cams = abc::cameras(r);
    CHECK_EQ(cams.size(), size_t(1));
    if (cams.empty()) return;
    abc::Matrix world;
    abc::Lens lens;
    CHECK(abc::cameraAt(r, *cams[0], 1.0 / 24.0, world, lens, error));
    CHECK(std::fabs(world[12]) < 1e-5 && std::fabs(world[13] - 1.5) < 1e-5 && std::fabs(world[14] - 6.0) < 1e-5);
    CHECK(std::fabs(lens.focalLength - 35.0) < 1e-9 && std::fabs(lens.verticalAperture - 2.4) < 1e-9);
    double first = 0.0, last = 0.0;
    CHECK(abc::timeRange(r, first, last));
    CHECK(std::fabs(first - 1.0 / 24.0) < 1e-9 && std::fabs(last - 4.0 / 24.0) < 1e-9);

    // A waved grid, a grid built face by face, particles.
    abc::ArchiveReader d;
    CHECK(d.open(kData + "deform.abc", error));
    auto count = [&](double t, const std::string& path) {
        abc::ImportOptions o;
        o.roots = {path};
        return abc::importGeometry(d, t, o);
    };
    const auto w1 = count(1.0 / 24.0, "/wave/Grid"), w3 = count(3.0 / 24.0, "/wave/Grid");
    CHECK(w1->pointCount() == 49 && w3->pointCount() == 49);  // 7 x 7
    CHECK(w1->positions()[10] != w3->positions()[10]);  // the wave moves the points
    CHECK(count(1.0 / 24.0, "/built")->primitiveCount() < count(3.0 / 24.0, "/built")->primitiveCount());
    const auto parts = count(3.0 / 24.0, "/wave/parts");
    CHECK(parts->pointCount() > 0 && parts->points().find("id") != nullptr);
    CHECK(abc::geometryVaries(d, abc::ImportOptions{}));
}

TEST(alembic_export_moves_and_turns_each_body_as_the_pieces_are_posed) {
    CHECK(sim::rigidAvailable());
    sim::RigidScene scene;
    scene.pieces = fracturedBox(Vec3(0.0f, 2.0f, 0.0f), Vec3(1.0f, 1.0f, 1.0f), 6);
    scene.solver.glue = 0.0f;  // loose: they fall and tumble
    sim::RigidSolver solver(scene);
    CHECK(solver.error().empty());
    TempFolder dir("abc_bodies");
    sim::AbcExport out(dir / "shot.abc", "ground", 30.0f);
    // A still ground as the displayed geometry; a camera that moves.
    const auto ground = sampleGeometry();
    std::string error;
    sim::Frame last;
    for (int f = 1; f <= 20; ++f) {
        solver.step();
        sim::Frame frame;
        frame.number = f;
        frame.rigid = solver.capture();
        sim::Camera camera;
        camera.position = Vec3(0.0f, 1.0f, 8.0f + 0.1f * static_cast<float>(f));
        camera.rotation = Vec3(0.0f, 0.0f, 0.0f);
        camera.focal = 35.0f;
        CHECK(out.add(frame, ground, &camera, sim::Look(), error));
        last = frame;
    }
    CHECK(out.finish(error));
    const std::shared_ptr<const sim::RigidLayout> layout = sim::rigidLayout(*scene.pieces, "piece");
    CHECK_EQ(out.bodies(), layout->bodies);
    CHECK_EQ(out.frames(), 20);

    abc::ArchiveReader r;
    CHECK(r.open(dir / "shot.abc", error));
    CHECK_EQ(r.meta().at("FramesPerTimeUnit"), std::string("30.000000"));
    // At frame 20, every body's points where the pieces are posed.
    const double time = 20.0 / 30.0;
    abc::ImportOptions pieces;
    pieces.roots = {"/pieces"};
    const auto back = abc::importGeometry(r, time, pieces);
    const sim::Look look;
    const GeometryPtr drawn = sim::drawnPieces(last.rigid, look.piecesColor, look.piecesInside, look.insideGroup, look.rebarColor);
    const auto Q = drawn->positions();
    for (int b = 0; b < layout->bodies; ++b) {
        char name[48];
        std::snprintf(name, sizeof name, "/pieces/body_%04d/body_%04dShape", b, b);
        // The body's points in the order of their numbers, as written.
        std::vector<uint32_t> source;
        for (const uint32_t prim : layout->prims[static_cast<size_t>(b)]) {
            for (const uint32_t p : drawn->primitivePoints(prim)) source.push_back(p);
        }
        std::sort(source.begin(), source.end());
        source.erase(std::unique(source.begin(), source.end()), source.end());
        std::vector<Vec3> read;
        std::vector<uint8_t> seen(back->pointCount(), 0);
        for (size_t p = 0; p < back->primitiveCount(); ++p) {
            if (primitiveString(*back, "path", p) != name) continue;
            for (const uint32_t q : back->primitivePoints(p)) seen[q] = 1;
        }
        for (size_t q = 0; q < seen.size(); ++q) {
            if (seen[q]) read.push_back(back->positions()[q]);
        }
        CHECK_EQ(read.size(), source.size());
        for (size_t i = 0; i < read.size() && i < source.size(); ++i) CHECK(length(read[i] - Q[source[i]]) < 1e-4f);
    }
    // The faces the fracture cut: a face set of each body, a group read back.
    const auto all = abc::importGeometry(r, time, abc::ImportOptions{});
    CHECK(all->findGroup("inside") != nullptr);
    // The ground, written once: its points' one sample serves every frame.
    const abc::ObjectReader* mesh = nullptr;
    for (const abc::ObjectReader* o : r.objects()) {
        if (o->path == "/ground/mesh") mesh = o;
    }
    CHECK(mesh != nullptr);
    if (mesh) {
        const abc::PropertyReader* P = mesh->properties.find(".geom")->find("P");
        CHECK(P && P->header.samples == 20 && P->header.constant());
    }
    // The camera: moving back a tenth a frame, its lens 24 mm high.
    const auto cams = abc::cameras(r);
    CHECK_EQ(cams.size(), size_t(1));
    if (cams.empty()) return;
    abc::Matrix world;
    abc::Lens lens;
    CHECK(abc::cameraAt(r, *cams[0], time, world, lens, error));
    CHECK(std::fabs(world[14] - 10.0) < 1e-5);
    CHECK(std::fabs(lens.focalLength - 35.0) < 1e-6 && std::fabs(lens.verticalAperture - 2.4) < 1e-9);
}

TEST(alembic_export_hides_a_body_blown_to_dust) {
    CHECK(sim::rigidAvailable());
    Geometry block = *fracturedBox(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 1.0f, 1.0f), 1);
    {
        auto release = block.primitives().create("release", AttrType::Float).write<float>();
        std::fill(release.begin(), release.end(), 0.1f);
        auto vanish = block.primitives().create("vanish", AttrType::Int).write<int32_t>();
        std::fill(vanish.begin(), vanish.end(), 1);
    }
    sim::RigidScene scene;
    scene.pieces = std::make_shared<Geometry>(block);
    sim::RigidSolver solver(scene);
    TempFolder dir("abc_dust");
    sim::AbcExport out(dir / "dust.abc");
    std::string error;
    int gone = 0;
    for (int f = 1; f <= 12; ++f) {
        solver.step();
        sim::Frame frame;
        frame.number = f;
        frame.rigid = solver.capture();
        if (!frame.rigid.vanished.empty() && gone == 0) gone = f;
        CHECK(out.add(frame, nullptr, nullptr, sim::Look(), error));
    }
    CHECK(out.finish(error));
    CHECK(gone > 1);
    abc::ArchiveReader r;
    CHECK(r.open(dir / "dust.abc", error));
    abc::ImportOptions o;
    o.roots = {"/pieces"};
    CHECK(abc::importGeometry(r, 1.0 / 30.0, o)->pointCount() > 0);
    CHECK_EQ(abc::importGeometry(r, gone / 30.0, o)->pointCount(), size_t(0));
    o.hidden = true;
    CHECK(abc::importGeometry(r, gone / 30.0, o)->pointCount() > 0);
    // The grit it threw: points, numbered.
    const auto grit = abc::importGeometry(r, 12.0 / 30.0, abc::ImportOptions{});
    CHECK(grit->points().find("id") != nullptr);
}

TEST(alembic_import_node_cooks_the_archive_at_the_frame) {
    sim::Network net;
    const int n = net.add("alembic_import");
    CHECK(net.setText(n, "file", kData + "shapes.abc"));
    CHECK(net.setText(n, "objects", "/spinner"));
    sim::GeometryGraph graph;
    graph.sync(net);
    // 24 frames a second, as Blender wrote it: frame 4 is the spinner's last.
    auto centre = [&](int frame) {
        const GeometryPtr g = graph.cook(n, frame, 1.0f / 24.0f);
        Vec3 c(0.0f);
        for (const Vec3& p : g->positions()) c += p;
        return g->pointCount() ? c / static_cast<float>(g->pointCount()) : c;
    };
    CHECK(length(centre(1) - Vec3(2.0f, 0.0f, 0.0f)) < 1e-4f);
    CHECK(length(centre(4) - Vec3(3.0f, 0.0f, 0.0f)) < 1e-4f);
    CHECK_EQ(graph.cook(n, 1, 1.0f / 24.0f)->primitiveCount(), size_t(6));
    CHECK_EQ(graph.error(n), std::string());
    // At 12 frames a second, frame 2 is the file's frame 4.
    CHECK(length(graph.cook(n, 2, 1.0f / 12.0f)->positions()[0] - graph.cook(n, 4, 1.0f / 24.0f)->positions()[0]) < 1e-5f);
    // A file that is not there.
    CHECK(net.setText(n, "file", kData + "nowhere.abc"));
    graph.sync(net);
    CHECK_EQ(graph.cook(n, 1, 1.0f / 24.0f)->pointCount(), 0u);
    CHECK(graph.error(n).find("nowhere.abc") != std::string::npos);
}

TEST(alembic_camera_node_is_the_shots_camera) {
    sim::Network net;
    const int cam = net.add("alembic_camera");
    const int out = net.add("output");
    CHECK(net.setText(cam, "file", kData + "shapes.abc"));
    net.setParam(cam, "width", "1200");
    net.setParam(out, "fps", "24");
    net.setParam(out, "frames", "4");
    CHECK(net.connect(cam, "camera", out, "camera"));
    const sim::Compiled c = net.compile();
    CHECK(c.hasCamera);
    CHECK(c.fileAnimation);
    // Blender's camera stood at (0, -6, 1.5), Z up, and moved 1.5 along x.
    CHECK(length(c.cameraAt(1).position - Vec3(0.0f, 1.5f, 6.0f)) < 1e-4f);
    CHECK(length(c.cameraAt(4).position - Vec3(1.5f, 1.5f, 6.0f)) < 1e-4f);
    // Tipped 80 degrees up from looking down: along -z, ten degrees down.
    const Vec3 forward = c.cameraAt(1).forward();
    CHECK(forward.z < -0.95f && forward.y < -0.1f && forward.y > -0.25f);
    // The picture shaped as its 36 x 24 mm back; a 35 mm lens as wide.
    CHECK_EQ(c.cameraAt(1).height, 800);
    CHECK(std::fabs(c.cameraAt(1).focal - 35.0f) < 1e-3f);
    // An object that is no camera says so.
    CHECK(net.setText(cam, "object", "/box"));
    bool said = false;
    for (const sim::Problem& p : net.compile().problems) said = said || p.message.find("not a camera") != std::string::npos;
    CHECK(said);
}
