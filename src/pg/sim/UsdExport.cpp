#include "pg/sim/UsdExport.h"

#include "pg/io/Vdb.h"
#include "pg/sim/Rigid.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <utility>
#include <vector>

namespace pg::sim {

namespace fs = std::filesystem;
namespace usda = io::usda;
using usda::Prim;

namespace {

const Vec3 kGrey(0.72f, 0.72f, 0.74f);  // what the viewport draws what has no colour in
const char* const kSurface = "</World/Looks/surface>";
const char* const kBinding = "prepend apiSchemas = [\"MaterialBindingAPI\"]";

std::string four(int frame) {
    char digits[16];
    std::snprintf(digits, sizeof digits, "%04d", frame);
    return digits;
}

}  // namespace

struct UsdExport::Impl {
    std::string path, geometryName, stem;
    fs::path gasFolder;
    int frames = 0, first = 0, last = 0;
    usda::Bounds scene;  // what is in it, for the size of the ground

    // The displayed geometry, a frame each; whether it ever changes.
    std::vector<std::pair<int, GeometryPtr>> geometry;
    bool geometryChanges = false;

    // The bodies of the RBD Solver: their shapes at rest, coloured as they
    // are drawn, about their middles; a pose a frame -- where the middle
    // is, and how the body is turned; the frame each is blown away at.
    GeometryPtr drawn;
    std::shared_ptr<const RigidLayout> layout;
    std::string insideGroup;
    std::vector<Vec3> middles;
    std::vector<int> bodyFrames;
    std::vector<std::vector<std::array<float, 7>>> motion;
    std::vector<int> gone;
    struct Grit {
        int frame = 0;
        std::vector<float> bits, velocity;  // x, y, z and size of each bit; how fast it goes
        std::vector<uint32_t> ids;
    };
    std::vector<Grit> grit;
    Vec3 gritColor;

    // The gas: a file a frame, and the box it fills.
    std::vector<std::pair<int, std::string>> gasFiles, gasExtent;

    std::vector<std::pair<int, Camera>> cameras;
    std::vector<std::pair<int, Look>> looks;

    void startBodies(const RigidFrame& r, const Look& look) {
        layout = r.layout ? r.layout : rigidLayout(*r.pieces, r.attribute);
        // At rest, coloured as the look draws them: no pose moves them, none
        // is gone, no grit.
        RigidFrame rest;
        rest.pieces = r.pieces;
        rest.layout = layout;
        rest.attribute = r.attribute;
        drawn = drawnPieces(rest, look.piecesColor, look.piecesInside, look.insideGroup);
        insideGroup = look.insideGroup;
        gritColor = look.piecesInside * 0.9f;
        const auto P = drawn->positions();
        middles.assign(static_cast<size_t>(layout->bodies), Vec3());
        for (int b = 0; b < layout->bodies; ++b) {
            usda::Bounds box;
            for (const uint32_t prim : layout->prims[static_cast<size_t>(b)]) {
                for (const uint32_t p : drawn->primitivePoints(prim)) box.grow(P[p]);
            }
            if (!box.empty()) middles[static_cast<size_t>(b)] = (box.lo + box.hi) * 0.5f;
            scene.grow(box);
        }
        motion.assign(middles.size(), {});
        gone.assign(middles.size(), INT_MAX);
    }
};

UsdExport::UsdExport(std::string path, std::string geometryName) : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    impl_->geometryName = usda::identifier(geometryName.empty() ? "geometry" : geometryName);
    // Not the name of another prim of the stage.
    for (const char* taken : {"Looks", "pieces", "grit", "gas", "camera", "sun", "sky", "ground"}) {
        if (impl_->geometryName == taken) impl_->geometryName += "_geometry";
    }
    const fs::path p(impl_->path);
    impl_->stem = p.stem().string();
    impl_->gasFolder = p.parent_path() / (impl_->stem + "_gas");
}

UsdExport::~UsdExport() = default;

const std::string& UsdExport::path() const { return impl_->path; }
int UsdExport::frames() const { return impl_->frames; }
int UsdExport::bodies() const { return static_cast<int>(impl_->middles.size()); }
int UsdExport::gasFiles() const { return static_cast<int>(impl_->gasFiles.size()); }

bool UsdExport::add(const Frame& frame, const GeometryPtr& geometry, const Camera* camera, const Look& look,
                    std::string& error) {
    Impl& m = *impl_;
    const int f = frame.number;
    if (m.frames == 0) m.first = f;
    m.last = f;
    ++m.frames;

    // The displayed geometry: whether it changed since the frame before.
    if (!m.geometry.empty()) {
        const GeometryPtr& before = m.geometry.back().second;
        if (before != geometry && (!before || !geometry || before->hash() != geometry->hash())) m.geometryChanges = true;
    }
    if (geometry && (m.geometry.empty() || m.geometryChanges)) {
        for (const Vec3& p : geometry->positions()) m.scene.grow(p);
    }
    m.geometry.emplace_back(f, geometry);

    // The bodies: a pose each.
    const RigidFrame& r = frame.rigid;
    if (!r.empty() && r.pieces) {
        if (!m.layout) m.startBodies(r, look);
        m.bodyFrames.push_back(f);
        for (size_t b = 0; b < m.motion.size(); ++b) {
            const RigidPose pose = b < r.poses.size() ? r.poses[b] : RigidPose{};
            const Vec3 at = pose.apply(m.middles[b]);
            m.motion[b].push_back({at.x, at.y, at.z, pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w});
        }
        for (const uint32_t v : r.vanished) {
            if (v < m.gone.size()) m.gone[v] = std::min(m.gone[v], f);
        }
        m.grit.push_back({f, r.debris, r.debrisVelocity, r.debrisIds});
    }

    // The gas: a file of it.
    const Domain& d = frame.domain;
    const size_t cells = d.cellCount();
    if (!frame.fields.empty() && frame.fields.size() >= 3 * cells) {
        const char* names[3] = {"density", "temperature", "flame"};
        std::vector<Volume> volumes;
        for (int channel = 0; channel < 3; ++channel) {
            std::vector<float> values(cells);
            for (size_t c = 0; c < cells; ++c) values[c] = fromHalf(frame.fields[3 * c + static_cast<size_t>(channel)]);
            volumes.push_back(Volume::make(names[channel], d.origin(), d.voxel, d.cells[0], d.cells[1], d.cells[2],
                                           std::move(values)));
        }
        std::error_code ec;
        fs::create_directories(m.gasFolder, ec);
        const std::string name = m.stem + "_gas." + four(f) + ".vdb";
        if (!io::writeVdb(volumes, (m.gasFolder / name).string(), error)) return false;
        m.gasFiles.emplace_back(f, usda::asset("./" + m.stem + "_gas/" + name));
        usda::Bounds box;
        box.grow(d.origin());
        box.grow(d.origin() + d.size());
        m.gasExtent.emplace_back(f, box.extent());
        m.scene.grow(box);
    }

    if (camera) m.cameras.emplace_back(f, *camera);
    m.looks.emplace_back(f, look);
    return true;
}

usda::Stage UsdExport::stage(float fps) const {
    const Impl& m = *impl_;
    usda::Stage s;
    // 30, not the 29.999998 that 1 / (1 / 30.0f) comes to; 23.976 stays.
    const std::string rate = usda::number(std::fabs(fps - std::round(fps)) < 1e-3f ? std::round(fps) : fps);
    s.metadata = {{"defaultPrim", usda::quoted("World")},
                  {"doc", usda::quoted("Written by Prototype")},
                  {"endTimeCode", std::to_string(m.last)},
                  {"framesPerSecond", rate},
                  {"metersPerUnit", "1"},
                  {"startTimeCode", std::to_string(m.first)},
                  {"timeCodesPerSecond", rate},
                  {"upAxis", usda::quoted("Y")}};
    Prim& world = s.prims.emplace_back("Xform", "World");
    std::vector<int32_t> local;  // scratch for meshText

    // The material: the colour of displayColor, a little rough.
    {
        Prim& looks = world.child("Scope", "Looks");
        Prim& material = looks.child("Material", "surface");
        material.set("token", "outputs:surface.connect", "</World/Looks/surface/shader.outputs:surface>");
        Prim& shader = material.child("Shader", "shader");
        shader.setUniform("token", "info:id", usda::quoted("UsdPreviewSurface"));
        shader.set("color3f", "inputs:diffuseColor.connect", "</World/Looks/surface/color.outputs:result>");
        shader.set("float", "inputs:roughness", "0.85");
        shader.set("token", "outputs:surface", "");
        Prim& color = material.child("Shader", "color");
        color.setUniform("token", "info:id", usda::quoted("UsdPrimvarReader_float3"));
        color.set("float3", "inputs:fallback", usda::tuple(kGrey));
        color.set("string", "inputs:varname", usda::quoted("displayColor"));
        color.set("float3", "outputs:result", "");
    }

    // The displayed geometry: a frame of it each when it changes, else the
    // first.
    std::vector<std::pair<int, const Geometry*>> shown;
    for (const auto& [f, geo] : m.geometry) {
        if (geo && (m.geometryChanges || shown.empty())) shown.emplace_back(f, geo.get());
    }
    if (!shown.empty()) {
        Prim& g = world.children.emplace_back(usda::geometryPrim(m.geometryName, shown));
        g.metadata.push_back(kBinding);
        g.relate("material:binding", kSurface);
    }

    // The bodies: a shape each, moved and turned.
    if (m.layout && !m.bodyFrames.empty()) {
        Prim& pieces = world.child("Xform", "pieces");
        pieces.metadata.push_back(kBinding);
        pieces.relate("material:binding", kSurface);
        const Group* inside = m.drawn->findGroup(m.insideGroup);
        if (inside && inside->classOf() != AttrClass::Primitive) inside = nullptr;
        // body_0000, body_0001...: as many digits as the last needs, at least four.
        const size_t digits = std::max<size_t>(4, std::to_string(m.middles.size()).size());
        for (size_t b = 0; b < m.middles.size(); ++b) {
            const std::string number = std::to_string(b);
            Prim& body = pieces.child("Xform", "body_" + std::string(digits - number.size(), '0') + number);
            std::vector<std::pair<int, std::string>> at, turned;
            at.reserve(m.bodyFrames.size());
            turned.reserve(m.bodyFrames.size());
            for (size_t i = 0; i < m.bodyFrames.size(); ++i) {
                const auto& q = m.motion[b][i];
                at.emplace_back(m.bodyFrames[i], usda::tuple(Vec3(q[0], q[1], q[2])));
                turned.emplace_back(m.bodyFrames[i], usda::quat(Vec4(q[3], q[4], q[5], q[6])));
            }
            usda::animate(body, "double3", "xformOp:translate", at);
            usda::animate(body, "quatf", "xformOp:orient", turned);
            body.setUniform("token[]", "xformOpOrder", usda::tokens({"xformOp:translate", "xformOp:orient"}));
            if (m.gone[b] != INT_MAX) {
                usda::Attribute& seen = body.set("token", "visibility", "");
                seen.samples = {{m.first, usda::quoted("inherited")}, {m.gone[b], usda::quoted("invisible")}};
            }
            const usda::MeshText shape = usda::meshText(*m.drawn, m.layout->prims[b], m.middles[b], inside, local);
            Prim mesh = usda::meshPrim("mesh", {{m.first, shape}});
            if (!shape.inside.empty()) {
                mesh.setUniform("token", "subsetFamily:materialBind:familyType", usda::quoted("nonOverlapping"));
                Prim& cut = mesh.child("GeomSubset", "inside");
                cut.setUniform("token", "elementType", usda::quoted("face"));
                cut.setUniform("token", "familyName", usda::quoted("materialBind"));
                cut.set("int[]", "indices", usda::integers(shape.inside));
            }
            body.children.push_back(std::move(mesh));
        }
        // The grit: its velocity and its numbers where the frames have them --
        // a renderer blurs a bit by them as it flies.
        const bool anyGrit = std::any_of(m.grit.begin(), m.grit.end(), [](const Impl::Grit& g) { return !g.bits.empty(); });
        if (anyGrit) {
            Prim& grit = world.child("Points", "grit");
            grit.metadata.push_back(kBinding);
            const bool moving = std::all_of(m.grit.begin(), m.grit.end(), [](const Impl::Grit& g) {
                return g.velocity.size() == 3 * (g.bits.size() / 4);
            });
            const bool numbered = std::all_of(m.grit.begin(), m.grit.end(), [](const Impl::Grit& g) {
                return g.ids.size() == g.bits.size() / 4;
            });
            std::vector<std::pair<int, std::string>> extent, points, widths, velocities, ids;
            for (const Impl::Grit& g : m.grit) {
                const size_t n = g.bits.size() / 4;
                std::vector<Vec3> at, v;
                std::vector<float> size;
                std::vector<int32_t> number;
                usda::Bounds box;
                float widest = 0.0f;
                for (size_t i = 0; i < n; ++i) {
                    at.emplace_back(g.bits[4 * i], g.bits[4 * i + 1], g.bits[4 * i + 2]);
                    size.push_back(g.bits[4 * i + 3]);
                    box.grow(at.back());
                    widest = std::max(widest, g.bits[4 * i + 3]);
                    if (moving) v.emplace_back(g.velocity[3 * i], g.velocity[3 * i + 1], g.velocity[3 * i + 2]);
                    if (numbered) number.push_back(static_cast<int32_t>(g.ids[i]));
                }
                extent.emplace_back(g.frame, box.extent(0.5f * widest));
                points.emplace_back(g.frame, usda::tuples(at));
                widths.emplace_back(g.frame, usda::numbers(size));
                if (moving) velocities.emplace_back(g.frame, usda::tuples(v));
                if (numbered) ids.emplace_back(g.frame, usda::integers(number));
            }
            usda::animate(grit, "float3[]", "extent", extent);
            usda::animate(grit, "int64[]", "ids", ids);
            usda::animate(grit, "point3f[]", "points", points);
            grit.set("color3f[]", "primvars:displayColor", usda::tuples(std::span<const Vec3>(&m.gritColor, 1))).metadata =
                usda::interpolation("constant");
            usda::animate(grit, "vector3f[]", "velocities", velocities);
            usda::animate(grit, "float[]", "widths", widths, usda::interpolation("vertex"));
            grit.relate("material:binding", kSurface);
        }
    }

    // The gas: its fields in the files beside the stage.
    if (!m.gasFiles.empty()) {
        Prim& gas = world.child("Volume", "gas");
        usda::animate(gas, "float3[]", "extent", m.gasExtent);
        for (const char* field : {"density", "temperature", "flame"}) {
            gas.relate(std::string("field:") + field, std::string("</World/gas/") + field + ">");
            Prim& asset = gas.child("OpenVDBAsset", field);
            usda::animate(asset, "asset", "filePath", m.gasFiles);
            asset.set("token", "fieldName", usda::quoted(field));
        }
    }

    // The camera: where it is and how it is turned -- degrees about x, then
    // y, then z, as the Camera node has them -- and its lens: 24 mm of
    // film from top to bottom, in tenths of a unit.
    if (!m.cameras.empty()) {
        Prim& cam = world.child("Camera", "camera");
        std::vector<std::pair<int, std::string>> at, turned, focal, across, exposure;
        for (const auto& [f, c] : m.cameras) {
            at.emplace_back(f, usda::tuple(c.position));
            turned.emplace_back(f, usda::tuple(c.rotation));
            focal.emplace_back(f, usda::number(c.focal / 100.0f));
            across.emplace_back(f, usda::number(0.24f * c.aspect()));
        }
        for (const auto& [f, look] : m.looks) exposure.emplace_back(f, usda::number(std::log2(std::max(look.exposure, 1e-6f))));
        cam.set("float2", "clippingRange", "(0.05, 20000)");
        usda::animate(cam, "float", "exposure", exposure);
        usda::animate(cam, "float", "focalLength", focal);
        usda::animate(cam, "float", "horizontalAperture", across);
        cam.set("float", "verticalAperture", usda::number(0.24f));
        usda::animate(cam, "double3", "xformOp:translate", at);
        usda::animate(cam, "float3", "xformOp:rotateXYZ", turned);
        cam.setUniform("token[]", "xformOpOrder", usda::tokens({"xformOp:translate", "xformOp:rotateXYZ"}));
    }

    // The light: the sun shines along its -z, from where the look has it;
    // the sky all round.
    if (!m.looks.empty()) {
        Prim& sun = world.child("DistantLight", "sun");
        Prim& sky = world.child("DomeLight", "sky");
        std::vector<std::pair<int, std::string>> sunColor, sunIntensity, sunTurned, skyColor, skyIntensity;
        Vec3 near;
        for (const auto& [f, look] : m.looks) {
            sunColor.emplace_back(f, usda::tuple(look.lightColor));
            sunIntensity.emplace_back(f, usda::number(look.lightIntensity));
            near = Camera::rotationFor(look.lightDirection() * -1.0f, Vec3(0.0f, 1.0f, 0.0f), near);
            sunTurned.emplace_back(f, usda::tuple(near));
            skyColor.emplace_back(f, usda::tuple(look.skyColor));
            skyIntensity.emplace_back(f, usda::number(look.skyIntensity));
        }
        sun.set("float", "inputs:angle", "0.53");
        usda::animate(sun, "color3f", "inputs:color", sunColor);
        usda::animate(sun, "float", "inputs:intensity", sunIntensity);
        usda::animate(sun, "float3", "xformOp:rotateXYZ", sunTurned);
        sun.setUniform("token[]", "xformOpOrder", usda::tokens({"xformOp:rotateXYZ"}));
        usda::animate(sky, "color3f", "inputs:color", skyColor);
        usda::animate(sky, "float", "inputs:intensity", skyIntensity);

        // The floor: a square round what is in the scene, in the look's
        // ground colour.
        const Look& look = m.looks.front().second;
        if (look.floor) {
            usda::Bounds box = m.scene;
            if (box.empty()) box.grow(Vec3());
            const Vec3 middle = (box.lo + box.hi) * 0.5f;
            const float half = std::max(20.0f, 2.0f * std::max(box.hi.x - box.lo.x, box.hi.z - box.lo.z));
            const float x0 = middle.x - half, x1 = middle.x + half, z0 = middle.z - half, z1 = middle.z + half;
            const std::vector<Vec3> corners = {{x0, 0.0f, z0}, {x0, 0.0f, z1}, {x1, 0.0f, z1}, {x1, 0.0f, z0}};
            Prim& ground = world.child("Mesh", "ground");
            ground.metadata.push_back(kBinding);
            ground.set("float3[]", "extent", "[" + usda::tuple(corners[0]) + ", " + usda::tuple(corners[2]) + "]");
            ground.set("int[]", "faceVertexCounts", "[4]");
            ground.set("int[]", "faceVertexIndices", "[0, 1, 2, 3]");
            ground.set("point3f[]", "points", usda::tuples(corners));
            ground.set("color3f[]", "primvars:displayColor", usda::tuples(std::span<const Vec3>(&look.groundColor, 1)))
                .metadata = usda::interpolation("constant");
            ground.setUniform("token", "subdivisionScheme", usda::quoted("none"));
            ground.relate("material:binding", kSurface);
        }
    }
    return s;
}

bool isUsdPath(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".usda" || ext == ".usd";
}

bool UsdExport::finish(float fps, std::string& error) {
    if (impl_->frames == 0) {
        error = "no frame to write to " + impl_->path;
        return false;
    }
    std::error_code ec;
    const fs::path folder = fs::path(impl_->path).parent_path();
    if (!folder.empty()) fs::create_directories(folder, ec);
    return usda::writeStage(stage(fps), impl_->path, error);
}

}  // namespace pg::sim
