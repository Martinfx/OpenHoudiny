#include "pg/sim/AbcExport.h"

#include "pg/abc/Geom.h"
#include "pg/core/Half.h"
#include "pg/io/Vdb.h"
#include "pg/sim/WaterMesh.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <map>

namespace pg::sim {

namespace fs = std::filesystem;

namespace {

constexpr float kDropWidth = 0.002f, kDropletWidth = 0.001f;

std::string four(int frame) {
    char digits[16];
    std::snprintf(digits, sizeof digits, "%04d", frame);
    return digits;
}

/// A name Alembic and the programs that read it take: letters, digits and
/// underscores, not starting with a digit.
std::string objectName(const std::string& s) {
    std::string out;
    for (const char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    return out;
}

/// The camera's matrix: its own x, y and z in the world, then where it is.
abc::Matrix cameraMatrix(const Camera& c) {
    const Rotation r = c.frame();
    abc::Matrix m = abc::identity();
    for (int a = 0; a < 3; ++a) {
        const Vec3& axis = r.axis(a);
        m[static_cast<size_t>(4 * a)] = axis.x;
        m[static_cast<size_t>(4 * a + 1)] = axis.y;
        m[static_cast<size_t>(4 * a + 2)] = axis.z;
    }
    m[12] = c.position.x;
    m[13] = c.position.y;
    m[14] = c.position.z;
    return m;
}

/// Its lens: the program's picture is 24 mm high.
abc::Lens cameraLens(const Camera& c) {
    abc::Lens l;
    l.focalLength = c.focal;
    l.verticalAperture = 2.4;
    l.horizontalAperture = 2.4 * static_cast<double>(c.aspect());
    return l;
}

/// Something of the shot that may come and go -- the water, the rain, the
/// displayed geometry: an Xform made the first frame it is there, its
/// shapes sampled from then on (empty where it is not there), and whether
/// it is seen at each frame of the shot -- written at the end.
struct Stream {
    std::string name;
    std::unique_ptr<abc::XformWriter> xform;
    std::vector<uint8_t> shown;
    std::map<std::string, std::unique_ptr<abc::MeshWriter>> meshes;
    std::map<std::string, std::unique_ptr<abc::CurvesWriter>> curves;
    std::map<std::string, std::unique_ptr<abc::PointsWriter>> points;
    std::map<std::string, size_t> samples;  // each shape's, so far
};

}  // namespace

struct AbcExport::Impl {
    std::string path, stem, geometryName;
    fs::path gasFolder;
    float fps = 30.0f;
    abc::ArchiveWriter archive;
    bool opened = false;
    int frames = 0, first = 0;
    std::vector<int> numbers;

    // The streams, by name, in the order they came.
    std::vector<std::unique_ptr<Stream>> streams;

    // The bodies: their shapes at rest, about their middles; a pose a frame;
    // the frame each comes (a fragment) and goes (blown to dust), and the
    // frame its pane of glass cracked.
    GeometryPtr drawn;
    std::shared_ptr<const RigidLayout> layout;
    std::string insideGroup;
    std::vector<Vec3> middles;
    std::vector<std::vector<std::array<float, 7>>> motion;
    std::vector<int> born, gone, cracked;
    int bodyFrames = 0, bodyFirst = 0;

    std::vector<std::pair<int, Camera>> cameras;
    int gasFiles = 0;

    /// A time sampling from frame `f` on, a frame each.
    uint32_t fromFrame(int f) { return archive.timeSampling(abc::TimeSampling::uniform(1.0 / fps, f / static_cast<double>(fps))); }

    Stream& stream(const std::string& name) {
        for (auto& s : streams) {
            if (s->name == name) return *s;
        }
        streams.push_back(std::make_unique<Stream>());
        streams.back()->name = name;
        return *streams.back();
    }

    /// The stream there at this frame: made now if it was not before.
    void show(Stream& s) {
        if (!s.xform) s.xform = std::make_unique<abc::XformWriter>(archive.top(), s.name, fromFrame(first));
        s.shown.resize(static_cast<size_t>(frames - 1), 0);
        s.shown.push_back(1);
    }

    /// A mesh of the stream: made at its first sample, with a time sampling
    /// from this frame; frames it missed since sampled empty.
    template <typename Writer, typename Sample>
    void shape(Stream& s, std::map<std::string, std::unique_ptr<Writer>>& writers, const std::string& name,
               const Sample& sample, int f) {
        auto& w = writers[name];
        size_t& done = s.samples[name];
        if (!w) {
            w = std::make_unique<Writer>(s.xform->object(), name, fromFrame(f));
            done = static_cast<size_t>(frames - 1);  // its first sample is this frame
        }
        while (done + 1 < static_cast<size_t>(frames)) {
            w->sample(Sample{});
            ++done;
        }
        w->sample(sample);
        ++done;
    }

    /// At the end of a frame: every shape made so far has a sample of it.
    void catchUp() {
        for (auto& s : streams) {
            auto pad = [&](auto& writers, auto empty) {
                for (auto& [name, w] : writers) {
                    size_t& done = s->samples[name];
                    while (done < static_cast<size_t>(frames)) {
                        w->sample(empty);
                        ++done;
                    }
                }
            };
            pad(s->meshes, abc::MeshSample{});
            pad(s->curves, abc::CurvesSample{});
            pad(s->points, abc::PointsSample{});
        }
    }

    /// The bodies of `r` not known yet: from frame `f` on (INT_MIN: from the
    /// start), at rest where they lie until then.
    void moreBodies(const RigidFrame& r, const Look& look, int f) {
        layout = r.layout ? r.layout : rigidLayout(*r.pieces, r.attribute);
        insideGroup = look.insideGroup;
        RigidFrame rest;
        rest.pieces = r.pieces;
        rest.layout = layout;
        rest.attribute = r.attribute;
        drawn = drawnPieces(rest, look.piecesColor, look.piecesInside, look.insideGroup);
        const auto P = drawn->positions();
        const size_t had = middles.size(), now = static_cast<size_t>(layout->bodies);
        middles.resize(now, Vec3());
        for (size_t b = had; b < now; ++b) {
            Vec3 lo(1e30f), hi(-1e30f);
            for (const uint32_t prim : layout->prims[b]) {
                for (const uint32_t p : drawn->primitivePoints(prim)) {
                    lo = min(lo, P[p]);
                    hi = max(hi, P[p]);
                }
            }
            if (lo.x <= hi.x) middles[b] = (lo + hi) * 0.5f;
        }
        motion.resize(now);
        for (size_t b = had; b < now; ++b) {
            motion[b].assign(static_cast<size_t>(bodyFrames), {middles[b].x, middles[b].y, middles[b].z, 0.0f, 0.0f, 0.0f, 1.0f});
        }
        born.resize(now, f);
        gone.resize(now, INT_MAX);
        cracked.resize(now, INT_MAX);
    }

    void writeBodies() {
        if (!layout || !drawn) return;
        const uint32_t ts = fromFrame(bodyFirst);
        abc::XformWriter pieces(archive.top(), "pieces", fromFrame(first));
        for (int i = 0; i < frames; ++i) pieces.sample(abc::identity());
        const size_t digits = std::max<size_t>(4, std::to_string(middles.size()).size());
        const AttributeArray* glass = drawn->primitives().find("glass");
        const Group* inside = drawn->findGroup(insideGroup);
        for (size_t b = 0; b < middles.size(); ++b) {
            const std::string number = std::to_string(b);
            const std::string name = "body_" + std::string(digits - number.size(), '0') + number;
            abc::XformWriter body(pieces.object(), name, ts);
            for (int i = 0; i < bodyFrames; ++i) {
                const int f = bodyFirst + i;
                const auto& q = motion[b][static_cast<size_t>(i)];
                body.sample(abc::pose(Vec4(q[3], q[4], q[5], q[6]), Vec3(q[0], q[1], q[2])), f >= born[b] && f < gone[b]);
            }
            body.finish();
            // Its shape about its middle, once; the cracks of its glass apart.
            std::vector<uint32_t> faces, cracks;
            for (const uint32_t prim : layout->prims[b]) (glassOf(glass, prim) >= 1.5f ? cracks : faces).push_back(prim);
            const abc::Matrix about = abc::pose(Vec4(0.0f, 0.0f, 0.0f, 1.0f), middles[b] * -1.0f);
            abc::MeshWriter mesh(body.object(), name + "Shape", ts);
            mesh.sample(abc::meshSample(*drawn, &about, faces));
            if (inside) {
                std::vector<int32_t> cut;
                int32_t k = 0;
                for (const uint32_t prim : faces) {
                    if (!drawn->primitiveClosed(prim) || drawn->primitiveVertexCount(prim) < 3) continue;
                    if (inside->contains(prim)) cut.push_back(k);
                    ++k;
                }
                if (!cut.empty()) abc::FaceSetWriter(mesh.object(), "inside", ts).sample(cut);
            }
            if (!cracks.empty()) {
                // Hidden until its pane breaks.
                abc::MeshWriter lines(body.object(), name + "Cracks", ts);
                lines.sample(abc::meshSample(*drawn, &about, cracks));
                abc::PropertyWriter& seen = lines.object().properties().scalar("visible", abc::Pod::I8, 1, ts);
                for (int i = 0; i < bodyFrames; ++i) {
                    const int8_t on = bodyFirst + i >= cracked[b] ? 1 : 0;
                    seen.scalar(&on);
                }
            }
        }
    }

    void writeCamera() {
        if (cameras.empty()) return;
        // The frames before the first with a camera: as that one.
        const uint32_t ts = fromFrame(first);
        abc::XformWriter rig(archive.top(), "camera", ts);
        abc::CameraWriter lens(rig.object(), "cameraShape", ts);
        size_t k = 0;
        for (const int f : numbers) {
            while (k + 1 < cameras.size() && cameras[k + 1].first <= f) ++k;
            rig.sample(cameraMatrix(cameras[k].second));
            lens.sample(cameraLens(cameras[k].second));
        }
        rig.finish();
    }
};

AbcExport::AbcExport(std::string path, std::string geometryName, float fps) : impl_(std::make_unique<Impl>()) {
    Impl& m = *impl_;
    m.path = std::move(path);
    m.fps = fps > 0.0f ? fps : 30.0f;
    m.geometryName = objectName(geometryName.empty() ? "geometry" : geometryName);
    for (const char* taken : {"pieces", "grit", "grains", "rebar", "cloth", "water", "rain", "camera"}) {
        if (m.geometryName == taken) m.geometryName += "_geometry";
    }
    const fs::path p(m.path);
    m.stem = p.stem().string();
    m.gasFolder = p.parent_path() / (m.stem + "_gas");
}

AbcExport::~AbcExport() = default;

const std::string& AbcExport::path() const { return impl_->path; }
int AbcExport::frames() const { return impl_->frames; }
int AbcExport::bodies() const { return static_cast<int>(impl_->middles.size()); }
int AbcExport::gasFiles() const { return impl_->gasFiles; }

bool AbcExport::add(const Frame& frame, const GeometryPtr& geometry, const Camera* camera, const Look& look,
                    std::string& error) {
    Impl& m = *impl_;
    const int f = frame.number;
    if (!m.opened) {
        char rate[32];
        std::snprintf(rate, sizeof rate, "%f", static_cast<double>(m.fps));
        if (!m.archive.open(m.path, {{"_ai_Application", "Prototype"}, {"_ai_Description", "a simulated shot"},
                                     {"FramesPerTimeUnit", rate}},
                            error)) {
            return false;
        }
        m.opened = true;
        m.first = f;
    }
    ++m.frames;
    m.numbers.push_back(f);

    // The displayed geometry: its polygons, lines and loose points.
    if (geometry) {
        Stream& s = m.stream(m.geometryName);
        m.show(s);
        // A sample the same as the last costs nothing more in the file.
        const abc::MeshSample mesh = abc::meshSample(*geometry);
        const abc::CurvesSample lines = abc::curvesSample(*geometry);
        const abc::PointsSample dots = abc::pointsSample(*geometry);
        if (!mesh.counts.empty() || s.meshes.count("mesh")) m.shape(s, s.meshes, "mesh", mesh, f);
        if (!lines.counts.empty() || s.curves.count("curves")) m.shape(s, s.curves, "curves", lines, f);
        if (!dots.P.empty() || s.points.count("points")) m.shape(s, s.points, "points", dots, f);
    }

    // The bodies: a pose each, kept; the grit and the bars as they are.
    const RigidFrame& r = frame.rigid;
    if (!r.empty() && r.pieces) {
        if (!m.layout) {
            m.bodyFirst = f;
            m.moreBodies(r, look, INT_MIN);
        } else if (r.layout && r.layout->bodies > m.layout->bodies) {
            m.moreBodies(r, look, f);
        }
        ++m.bodyFrames;
        for (size_t b = 0; b < m.motion.size(); ++b) {
            const RigidPose pose = b < r.poses.size() ? r.poses[b] : RigidPose{};
            const Vec3 at = pose.apply(m.middles[b]);
            m.motion[b].push_back({at.x, at.y, at.z, pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w});
        }
        for (const uint32_t v : r.vanished) {
            if (v < m.gone.size()) m.gone[v] = std::min(m.gone[v], f);
        }
        const std::vector<uint8_t> whole = wholePanes(r);
        for (size_t b = 0; b < m.cracked.size(); ++b) {
            if (b >= whole.size() || !whole[b]) m.cracked[b] = std::min(m.cracked[b], f);
        }
        if (!r.debris.empty()) {
            Stream& s = m.stream("grit");
            m.show(s);
            abc::PointsSample grit;
            const size_t n = r.debris.size() / 4;
            const Vec3 stone = look.piecesInside * 0.9f;
            for (size_t i = 0; i < n; ++i) {
                grit.P.emplace_back(r.debris[4 * i], r.debris[4 * i + 1], r.debris[4 * i + 2]);
                grit.widths.push_back(r.debris[4 * i + 3]);
                grit.ids.push_back(r.debrisIds.size() == n ? r.debrisIds[i] : i);
                grit.Cd.push_back(r.debrisGlass.size() == n && r.debrisGlass[i] ? kGlassChip : stone);
                if (r.debrisVelocity.size() == 3 * n) {
                    grit.v.emplace_back(r.debrisVelocity[3 * i], r.debrisVelocity[3 * i + 1], r.debrisVelocity[3 * i + 2]);
                }
            }
            m.shape(s, s.points, "gritShape", grit, f);
        }
        if (r.rebar) {
            const std::shared_ptr<Geometry> bars = rebarBars(r);
            if (bars->primitiveCount() > 0) {
                Stream& s = m.stream("rebar");
                m.show(s);
                abc::CurvesSample c = abc::curvesSample(*bars);
                if (c.widths.empty()) c.widths.assign(c.P.size(), 0.012f);
                c.Cd.assign(c.P.size(), look.rebarColor);
                m.shape(s, s.curves, "rebarShape", c, f);
            }
        }
    }

    // The water's surface -- unless the look hides it.
    if (!frame.water.empty() && look.waterSurface) {
        const std::shared_ptr<Geometry> surface = waterMesh(frame.water, &frame.rain);
        if (surface->primitiveCount() > 0) {
            Stream& s = m.stream("water");
            m.show(s);
            const std::string foam[1] = {"foam"};
            m.shape(s, s.meshes, "waterShape", abc::meshSample(*surface, nullptr, {}, foam), f);
        }
    }

    // The cloth -- when it is drawn -- in the look's colour where it has none.
    if (!frame.cloth.empty() && look.cloth) {
        const std::shared_ptr<Geometry> cloth = posedCloth(frame.cloth);
        if (cloth && cloth->primitiveCount() > 0) {
            if (!cloth->points().contains("Cd") && !cloth->vertices().contains("Cd") && !cloth->primitives().contains("Cd") &&
                !cloth->detail().contains("Cd")) {
                cloth->detail().create("Cd", AttrType::Vec3).write<Vec3>()[0] = look.clothColor;
            }
            Stream& s = m.stream("cloth");
            m.show(s);
            abc::MeshSample faces = abc::meshSample(*cloth);
            abc::CurvesSample ropes = abc::curvesSample(*cloth);
            if (!faces.counts.empty() || s.meshes.count("clothShape")) m.shape(s, s.meshes, "clothShape", faces, f);
            if (!ropes.counts.empty() || s.curves.count("ropes")) m.shape(s, s.curves, "ropes", ropes, f);
        }
    }

    // The grains -- when they are drawn.
    if (!frame.grains.empty() && frame.grains.fits() && look.grains) {
        const std::shared_ptr<Geometry> g = grainPoints(frame.grains, look.grainColor);
        Stream& s = m.stream("grains");
        m.show(s);
        abc::PointsSample grains = abc::pointsSample(*g);
        grains.ids.assign(frame.grains.ids.begin(), frame.grains.ids.end());
        m.shape(s, s.points, "grainsShape", grains, f);
    }

    // The rain: its drops and its droplets.
    if (frame.rain.dropCount() + frame.rain.dropletCount() > 0) {
        Stream& s = m.stream("rain");
        m.show(s);
        auto one = [&](const std::vector<float>& six, const std::vector<uint32_t>& ids, float width) {
            abc::PointsSample p;
            const size_t n = six.size() / 6;
            for (size_t i = 0; i < n; ++i) {
                const float* q = six.data() + 6 * i;
                p.P.emplace_back(q[0], q[1], q[2]);
                p.v.emplace_back(q[3], q[4], q[5]);
                p.ids.push_back(ids.size() == n ? ids[i] : i);
                p.widths.push_back(width);
            }
            return p;
        };
        m.shape(s, s.points, "drops", one(frame.rain.drops, frame.rain.dropIds, kDropWidth), f);
        m.shape(s, s.points, "droplets", one(frame.rain.droplets, frame.rain.dropletIds, kDropletWidth), f);
    }

    // The gas: a file of it beside the archive.
    const std::vector<Volume> volumes = gasVolumes(frame);
    if (!volumes.empty()) {
        std::error_code ec;
        fs::create_directories(m.gasFolder, ec);
        const std::string name = m.stem + "_gas." + four(f) + ".vdb";
        if (!io::writeVdb(volumes, (m.gasFolder / name).string(), error)) return false;
        ++m.gasFiles;
    }

    if (camera) m.cameras.emplace_back(f, *camera);
    m.catchUp();
    return true;
}

bool AbcExport::finish(std::string& error) {
    Impl& m = *impl_;
    if (!m.opened) {
        error = "no frame to write";
        return false;
    }
    // What is seen when: written now, for every frame of the shot.
    for (auto& s : m.streams) {
        if (!s->xform) continue;
        s->shown.resize(static_cast<size_t>(m.frames), 0);
        for (const uint8_t on : s->shown) s->xform->sample(abc::identity(), on != 0);
    }
    m.writeBodies();
    m.writeCamera();
    return m.archive.close(error);
}

bool isAlembicPath(const std::string& path) {
    const std::string ext = fs::path(path).extension().string();
    std::string lower;
    for (const char c : ext) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower == ".abc";
}

}  // namespace pg::sim
