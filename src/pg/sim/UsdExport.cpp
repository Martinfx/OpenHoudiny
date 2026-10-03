#include "pg/sim/UsdExport.h"

#include "pg/core/Instances.h"
#include "pg/io/Vdb.h"
#include "pg/sim/Cloth.h"
#include "pg/sim/Rigid.h"
#include "pg/sim/WaterMesh.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <utility>
#include <vector>

namespace pg::sim {

namespace fs = std::filesystem;
namespace usda = io::usda;
using usda::Prim;

namespace {

const Vec3 kGrey(0.72f, 0.72f, 0.74f);  // what the viewport draws what has no colour in
const char* const kSurface = "</World/Looks/surface>";
const char* const kGlass = "</World/Looks/glass>";
const char* const kWaterLook = "</World/Looks/water>";
const char* const kRainLook = "</World/Looks/rain>";
const char* const kBinding = "prepend apiSchemas = [\"MaterialBindingAPI\"]";
// How wide a drop and a droplet are drawn, metres: a renderer streaks them
// by their velocities.
constexpr float kDropWidth = 0.002f, kDropletWidth = 0.001f;

std::string four(int frame) {
    char digits[16];
    std::snprintf(digits, sizeof digits, "%04d", frame);
    return digits;
}

/// 30, not the 29.999998 that 1 / (1 / 30.0f) comes to; 23.976 stays.
std::string rateOf(float fps) {
    return usda::number(std::fabs(fps - std::round(fps)) < 1e-3f ? std::round(fps) : fps);
}

/// Quaternions x, y, z, w one after the other as quatf[]: [(w, x, y, z), ...].
std::string quaternions(const std::vector<float>& q) {
    std::string out = "[";
    out.reserve(q.size() * 12 + 2);
    for (size_t i = 0; i + 3 < q.size(); i += 4) {
        if (i > 0) out += ", ";
        out += usda::quat(Vec4(q[i], q[i + 1], q[i + 2], q[i + 3]));
    }
    return out + "]";
}

/// The particles' numbers as int64[]: whole, not as the int32 they would wrap to.
std::string idList(const std::vector<uint32_t>& ids) {
    std::string out = "[";
    out.reserve(ids.size() * 7 + 2);
    char buf[16];
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) out += ", ";
        const auto r = std::to_chars(buf, buf + sizeof buf, ids[i]);
        out.append(buf, r.ptr);
    }
    return out + "]";
}

std::vector<std::string> namesOf(const std::string& path) {
    std::vector<std::string> names;
    for (size_t at = 1; at <= path.size();) {
        const size_t end = std::min(path.find('/', at), path.size());
        if (end > at) names.push_back(path.substr(at, end - at));
        at = end + 1;
    }
    return names;
}

/// The prim at `path` ("/World/rain/drops") in `prims`, made -- an over, or
/// as `specifier` says -- where it is not there yet.
Prim& primAt(std::list<Prim>& prims, const std::string& path, const char* specifier = "over") {
    std::list<Prim>* level = &prims;
    Prim* at = nullptr;
    for (const std::string& name : namesOf(path)) {
        at = nullptr;
        for (Prim& p : *level) {
            if (p.name == name) at = &p;
        }
        if (!at) {
            at = &level->emplace_back("", name);
            at->specifier = specifier;
        }
        level = &at->children;
    }
    return *at;
}

/// `box` grown round `geo` as it is drawn: its instances by what they
/// stand for.
void growDrawn(usda::Bounds& box, const Geometry& geo) {
    Vec3 lo, hi;
    drawnBox(geo, lo, hi);
    if (lo.x > hi.x) return;
    box.grow(lo);
    box.grow(hi);
}

bool hasValues(const std::list<Prim>& prims) {
    for (const Prim& p : prims) {
        if (!p.attributes.empty() || hasValues(p.children)) return true;
    }
    return false;
}

/// Whether a frame's rain has drops or droplets to write.
bool raining(const RainFrame& r) { return r.dropCount() + r.dropletCount() > 0; }

}  // namespace

struct UsdExport::Impl {
    std::string path, geometryName, stem;
    float fps = 30.0f;
    fs::path gasFolder, framesFolder;
    int frames = 0, first = 0, last = 0, layers = 0;
    std::vector<int> numbers;  // every frame added
    usda::Bounds scene;        // what is in it, for the size of the ground

    // What takes its values from the frames' layers: a prim each, by its
    // path -- its type and the attributes the layers give it, declared (for
    // the stage and the manifest) with no value.
    struct Clipped {
        std::string type;
        std::map<std::string, usda::Field> declared;
    };
    std::map<std::string, Clipped> clipped;
    // ... the prototypes of each PointInstancer among them, by its path: held
    // in the stage, the first frame's that had instances.
    std::map<std::string, std::vector<std::shared_ptr<const Geometry>>> prototypes;
    // ... in sets, each on the prim it hangs from (/World/water, /World/rain):
    // the frames whose layers it takes, and the frames it is there at.
    struct ClipSet {
        std::vector<int> frames, present;
    };
    std::map<std::string, ClipSet> clipSets;

    // The displayed geometry: as it first showed -- written once, in the
    // stage, as long as it never changes; the layer of the frame it first
    // showed at is held back till then. When it does change, each frame it
    // changed at has it in its layer.
    GeometryPtr firstGeometry, lastGeometry;
    int firstGeometryFrame = 0;
    bool geometryChanges = false;
    std::unique_ptr<usda::Stage> held;
    std::vector<int> geometryPresent;

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
    /// The frame each body's pane of glass broke at -- its cracks there from
    /// then on (wholePanes); INT_MAX while it is whole.
    std::vector<int> cracked;
    Vec3 gritColor, rebarColor;

    // The gas: a file a frame, and the box it fills.
    std::vector<std::pair<int, std::string>> gasFiles, gasExtent;

    std::vector<std::pair<int, Camera>> cameras;
    std::vector<std::pair<int, Look>> looks;

    std::string geometryPath() const { return "/World/" + geometryName; }
    std::string layerFile(int frame) const { return stem + "." + four(frame) + ".usda"; }
    std::string layerAsset(int frame) const { return "./" + stem + "_frames/" + layerFile(frame); }
    std::string manifestAsset() const { return "./" + stem + "_frames/" + stem + ".manifest.usda"; }

    usda::Stage newLayer() const {
        usda::Stage layer;
        layer.metadata = {{"framesPerSecond", rateOf(fps)}, {"timeCodesPerSecond", rateOf(fps)}};
        return layer;
    }

    /// `fields` at frame f into `layer`, on the prim at `path` -- of `type`,
    /// which the stage declares them on.
    void sample(usda::Stage& layer, const std::string& at, const char* type, int f, std::vector<usda::Field> fields) {
        Prim& over = primAt(layer.prims, at);
        Clipped& c = clipped[at];
        c.type = type;
        for (usda::Field& field : fields) {
            if (c.declared.find(field.name) == c.declared.end()) {
                c.declared[field.name] = {field.type, field.name, field.metadata, ""};
            }
            usda::Attribute a;
            a.type = field.type;
            a.name = field.name;
            a.samples.emplace_back(f, std::move(field.value));
            over.attributes.push_back(std::move(a));
        }
    }

    /// Geometry at frame f under the prim `at`: its mesh, curves and points,
    /// and its instances, as geometryPrim makes them -- the instancer's
    /// prototypes those of the first frame with instances.
    void sampleShapes(usda::Stage& layer, const std::string& at, int f, const Geometry& whole) {
        std::shared_ptr<Geometry> rest;
        const Geometry* shapes = &whole;
        if (whole.prototypeCount() > 0) {
            rest = withoutInstances(whole);
            shapes = rest.get();
            const usda::InstancesText instances = usda::instancesText(whole);
            if (!instances.empty()) {
                prototypes.emplace(at + "/instances", whole.prototypes());
                sample(layer, at + "/instances", "PointInstancer", f, usda::fields(instances));
            }
        }
        const Geometry& geo = *shapes;
        std::vector<uint32_t> all(geo.primitiveCount());
        for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<uint32_t>(i);
        std::vector<int32_t> scratch;
        const usda::MeshText mesh = usda::meshText(geo, all, Vec3(), nullptr, scratch);
        const usda::CurvesText curves = usda::curvesText(geo);
        const usda::PointsText points = usda::pointsText(geo);
        if (!mesh.empty()) sample(layer, at + "/mesh", "Mesh", f, usda::fields(mesh));
        if (!curves.empty()) sample(layer, at + "/curves", "BasisCurves", f, usda::fields(curves));
        if (!points.empty()) sample(layer, at + "/points", "Points", f, usda::fields(points));
        primAt(layer.prims, at);  // there, even when it is empty now
    }

    /// The displayed geometry at frame f.
    void sampleGeometry(usda::Stage& layer, int f, const Geometry& geo) { sampleShapes(layer, geometryPath(), f, geo); }

    /// The cloth at frame f, where its points are -- torn, as it is torn --
    /// with their normals and velocities: its faces a Mesh, its ropes
    /// BasisCurves. What has no colour of its own is in the look's.
    bool sampleCloth(usda::Stage& layer, int f, const ClothFrame& c, const Vec3& color) {
        const std::shared_ptr<Geometry> cloth = posedCloth(c);
        if (!cloth || cloth->primitiveCount() == 0) return false;
        if (!cloth->points().contains("Cd") && !cloth->vertices().contains("Cd") && !cloth->primitives().contains("Cd") &&
            !cloth->detail().contains("Cd")) {
            cloth->detail().create("Cd", AttrType::Vec3).write<Vec3>()[0] = color;
        }
        // Not what only the solver reads: which points are pinned, how heavy
        // they are, how easily they tear.
        for (const char* solverOnly : {"pin", "mass", "tear"}) cloth->points().erase(solverOnly);
        sampleShapes(layer, "/World/cloth", f, *cloth);
        for (const Vec3& p : cloth->positions()) scene.grow(p);
        return true;
    }

    /// The grit at frame f: a point a bit, as wide as it is, with its
    /// velocity, its number and how it is turned -- the primvar orient --
    /// where the frame has them, and the primvar glass -- 1 for a chip of
    /// glass -- where any bit is one.
    void sampleGrit(usda::Stage& layer, int f, const RigidFrame& r) {
        const size_t n = r.debris.size() / 4;
        std::vector<Vec3> at, v;
        std::vector<float> size;
        usda::Bounds box;
        float widest = 0.0f;
        const bool moving = r.debrisVelocity.size() == 3 * n, numbered = r.debrisIds.size() == n;
        for (size_t i = 0; i < n; ++i) {
            at.emplace_back(r.debris[4 * i], r.debris[4 * i + 1], r.debris[4 * i + 2]);
            size.push_back(r.debris[4 * i + 3]);
            box.grow(at.back());
            widest = std::max(widest, r.debris[4 * i + 3]);
            if (moving) v.emplace_back(r.debrisVelocity[3 * i], r.debrisVelocity[3 * i + 1], r.debrisVelocity[3 * i + 2]);
        }
        std::vector<usda::Field> fields = {{"float3[]", "extent", "", box.extent(0.5f * widest)}};
        if (numbered) fields.push_back({"int64[]", "ids", "", idList(r.debrisIds)});
        fields.push_back({"point3f[]", "points", "", usda::tuples(at)});
        if (moving) fields.push_back({"vector3f[]", "velocities", "", usda::tuples(v)});
        fields.push_back({"float[]", "widths", usda::interpolation("vertex"), usda::numbers(size)});
        if (r.debrisGlass.size() == n && n > 0) {
            std::vector<uint32_t> glass(r.debrisGlass.begin(), r.debrisGlass.end());
            fields.push_back({"int[]", "primvars:glass", usda::interpolation("vertex"), idList(glass)});
        }
        // How each bit is turned: a stone copied onto it turns so.
        if (r.debrisOrient.size() == 4 * n && n > 0) {
            fields.push_back({"quatf[]", "primvars:orient", usda::interpolation("vertex"), quaternions(r.debrisOrient)});
        }
        sample(layer, "/World/grit", "Points", f, std::move(fields));
        scene.grow(box);
    }

    /// The grains at frame f: a point a grain, as wide as it is, with its
    /// velocity, its number, its colour -- the frame's, else the look's --
    /// and how it is turned (grainPoints): stones copied onto them turn so.
    void sampleGrains(usda::Stage& layer, int f, const GrainFrame& g, const Vec3& color) {
        const std::shared_ptr<Geometry> points = grainPoints(g, color);
        const size_t n = points->pointCount();
        const auto P = points->positions();
        const auto v = points->points().find("v")->read<Vec3>();
        const auto pscale = points->points().find("pscale")->read<float>();
        const auto cd = points->points().find("Cd")->read<Vec3>();
        const auto orient = points->points().find("orient")->read<Vec4>();
        std::vector<Vec3> at(P.begin(), P.end()), velocity(v.begin(), v.end()), colors(cd.begin(), cd.end());
        std::vector<float> width, turn;
        usda::Bounds box;
        float widest = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            width.push_back(2.0f * pscale[i]);
            widest = std::max(widest, width.back());
            box.grow(at[i]);
            turn.insert(turn.end(), {orient[i].x, orient[i].y, orient[i].z, orient[i].w});
        }
        std::vector<usda::Field> fields = {{"float3[]", "extent", "", box.extent(0.5f * widest)},
                                           {"int64[]", "ids", "", idList(g.ids)},
                                           {"point3f[]", "points", "", usda::tuples(at)},
                                           {"vector3f[]", "velocities", "", usda::tuples(velocity)},
                                           {"float[]", "widths", usda::interpolation("vertex"), usda::numbers(width)},
                                           {"color3f[]", "primvars:displayColor", usda::interpolation("vertex"),
                                            usda::tuples(colors)},
                                           {"quatf[]", "primvars:orient", usda::interpolation("vertex"), quaternions(turn)}};
        sample(layer, "/World/grains", "Points", f, std::move(fields));
        scene.grow(box);
    }

    /// The bars at frame f, where the pieces have taken them: a linear curve
    /// for each stretch of one, as thick as it is, and how fast each point
    /// goes. False if there are none.
    bool sampleRebar(usda::Stage& layer, int f, const RigidFrame& r) {
        const std::shared_ptr<Geometry> bars = rebarBars(r);
        if (bars->primitiveCount() == 0) return false;
        const auto P = bars->positions();
        const AttributeArray* wa = bars->points().find("width");
        const AttributeArray* va = bars->points().find("v");
        std::vector<Vec3> at, v;
        std::vector<float> widths;
        std::vector<int32_t> counts;
        usda::Bounds box;
        float widest = 0.0f;
        for (size_t prim = 0; prim < bars->primitiveCount(); ++prim) {
            const auto c = bars->primitivePoints(prim);
            counts.push_back(static_cast<int32_t>(c.size()));
            for (const uint32_t q : c) {
                at.push_back(P[q]);
                box.grow(P[q]);
                v.push_back(va ? va->read<Vec3>()[q] : Vec3());
                widths.push_back(wa ? wa->read<float>()[q] : 0.012f);
                widest = std::max(widest, widths.back());
            }
        }
        sample(layer, "/World/rebar", "BasisCurves", f,
               {{"int[]", "curveVertexCounts", "", usda::integers(counts)},
                {"float3[]", "extent", "", box.extent(0.5f * widest)},
                {"point3f[]", "points", "", usda::tuples(at)},
                {"vector3f[]", "velocities", "", usda::tuples(v)},
                {"float[]", "widths", usda::interpolation("vertex"), usda::numbers(widths)}});
        scene.grow(box);
        return true;
    }

    /// The water's surface at frame f; its colour is the stage's.
    void sampleWater(usda::Stage& layer, int f, const Geometry& mesh) {
        std::vector<uint32_t> all(mesh.primitiveCount());
        for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<uint32_t>(i);
        std::vector<int32_t> scratch;
        std::vector<usda::Field> fields = usda::fields(usda::meshText(mesh, all, Vec3(), nullptr, scratch));
        fields.erase(std::remove_if(fields.begin(), fields.end(),
                                    [](const usda::Field& x) { return x.name == "primvars:displayColor"; }),
                     fields.end());
        sample(layer, "/World/water", "Mesh", f, std::move(fields));
    }

    /// The rain at frame f: its drops and its droplets, each numbered and
    /// moving -- both, even when one of them has none now.
    void sampleRain(usda::Stage& layer, int f, const RainFrame& r) {
        auto one = [&](const char* name, const std::vector<float>& six, const std::vector<uint32_t>& ids, float width) {
            const size_t n = six.size() / 6;
            std::vector<Vec3> at(n), v(n);
            usda::Bounds box;
            for (size_t i = 0; i < n; ++i) {
                const float* p = six.data() + 6 * i;
                at[i] = Vec3(p[0], p[1], p[2]);
                v[i] = Vec3(p[3], p[4], p[5]);
                box.grow(at[i]);
            }
            std::vector<usda::Field> fields = {{"float3[]", "extent", "", box.extent(0.5f * width)}};
            if (ids.size() == n) fields.push_back({"int64[]", "ids", "", idList(ids)});
            fields.push_back({"point3f[]", "points", "", usda::tuples(at)});
            fields.push_back({"vector3f[]", "velocities", "", usda::tuples(v)});
            sample(layer, std::string("/World/rain/") + name, "Points", f, std::move(fields));
            scene.grow(box);
        };
        one("drops", r.drops, r.dropIds, kDropWidth);
        one("droplets", r.droplets, r.dropletIds, kDropletWidth);
    }

    /// A frame's layer, when it has values -- or when a clip names it, if
    /// only to say its prims have none then.
    bool writeLayer(const usda::Stage& layer, int f, bool named, std::string& error) {
        if (!named && !hasValues(layer.prims)) return true;
        std::error_code ec;
        fs::create_directories(framesFolder, ec);
        if (!usda::writeStage(layer, (framesFolder / layerFile(f)).string(), error)) return false;
        ++layers;
        return true;
    }

    /// visibility, where a prim is not there at every frame: invisible where
    /// it is not, written where that changes.
    void visibility(Prim& prim, const std::vector<int>& present) const {
        if (present.size() == numbers.size()) return;
        usda::Attribute& seen = prim.set("token", "visibility", "");
        size_t k = 0;
        int was = -1;
        for (const int f : numbers) {
            while (k < present.size() && present[k] < f) ++k;
            const int now = k < present.size() && present[k] == f ? 1 : 0;
            if (now != was) seen.samples.emplace_back(f, usda::quoted(now ? "inherited" : "invisible"));
            was = now;
        }
    }

    /// The prim `at` of the stage, its values from the frames' layers: the
    /// clips on it, and each prim under it the layers give values to,
    /// declared with their types and how they spread; invisible at the
    /// frames it is not `present` at.
    Prim& clippedPrim(Prim& world, const std::string& at, const char* type, const std::vector<int>& present) const {
        Prim& p = primAt(world.children, at.substr(std::string("/World").size()), "def");
        p.type = type;
        const ClipSet& set = clipSets.at(at);
        std::vector<std::pair<int, std::string>> assets;
        for (const int f : set.frames) assets.emplace_back(f, layerAsset(f));
        p.metadata.push_back(usda::clips(assets, manifestAsset(), at));
        visibility(p, present);
        for (const auto& [path, c] : clipped) {
            if (path != at && path.rfind(at + "/", 0) != 0) continue;
            Prim& q = path == at ? p : primAt(p.children, path.substr(at.size()), "def");
            q.type = c.type;
            for (const auto& [name, field] : c.declared) q.set(field.type, name, "").metadata = field.metadata;
            if (c.type == "Mesh") q.setUniform("token", "subdivisionScheme", usda::quoted("none"));
            if (c.type == "BasisCurves") {
                q.setUniform("token", "type", usda::quoted("linear"));
                if (!c.declared.count("widths")) q.set("float[]", "widths", "[0.01]").metadata = usda::interpolation("constant");
            }
            if (c.type == "PointInstancer" && prototypes.count(path)) usda::addPrototypes(q, path, prototypes.at(path));
        }
        return p;
    }

    usda::Stage manifest() const {
        usda::Stage s;
        for (const auto& [path, c] : clipped) {
            Prim& p = primAt(s.prims, path);
            for (const auto& [name, field] : c.declared) p.set(field.type, name, "");
        }
        return s;
    }

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
        rebarColor = look.rebarColor;
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
        cracked.assign(middles.size(), INT_MAX);
    }
};

UsdExport::UsdExport(std::string path, std::string geometryName, float fps) : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    impl_->fps = fps > 0.0f ? fps : 30.0f;
    impl_->geometryName = usda::identifier(geometryName.empty() ? "geometry" : geometryName);
    // Not the name of another prim of the stage.
    for (const char* taken :
         {"Looks", "pieces", "grit", "rebar", "cloth", "grains", "water", "rain", "gas", "camera", "sun", "sky",
          "ground"}) {
        if (impl_->geometryName == taken) impl_->geometryName += "_geometry";
    }
    const fs::path p(impl_->path);
    impl_->stem = p.stem().string();
    impl_->gasFolder = p.parent_path() / (impl_->stem + "_gas");
    impl_->framesFolder = p.parent_path() / (impl_->stem + "_frames");
}

UsdExport::~UsdExport() = default;

const std::string& UsdExport::path() const { return impl_->path; }
int UsdExport::frames() const { return impl_->frames; }
int UsdExport::bodies() const { return static_cast<int>(impl_->middles.size()); }
int UsdExport::gasFiles() const { return static_cast<int>(impl_->gasFiles.size()); }
int UsdExport::frameFiles() const { return impl_->layers; }
std::string UsdExport::frameFile(int frame) const { return impl_->layerAsset(frame); }

bool UsdExport::add(const Frame& frame, const GeometryPtr& geometry, const Camera* camera, const Look& look,
                    std::string& error) {
    Impl& m = *impl_;
    const int f = frame.number;
    if (m.frames == 0) m.first = f;
    m.last = f;
    ++m.frames;
    m.numbers.push_back(f);
    usda::Stage layer = m.newLayer();
    bool hold = false, named = false;  // held back; named by a clip

    // The displayed geometry: held back as it first shows; into the layer
    // of each frame it changes at.
    if (geometry) {
        m.geometryPresent.push_back(f);
        if (!m.firstGeometry) {
            m.firstGeometry = m.lastGeometry = geometry;
            m.firstGeometryFrame = f;
            growDrawn(m.scene, *geometry);
            hold = true;
        } else if (m.lastGeometry != geometry && m.lastGeometry->hash() != geometry->hash()) {
            Impl::ClipSet& set = m.clipSets[m.geometryPath()];
            if (!m.geometryChanges) {
                // It changes after all: the frame it first showed at has it
                // in its layer too.
                m.geometryChanges = true;
                if (m.held) {
                    m.sampleGeometry(*m.held, m.firstGeometryFrame, *m.firstGeometry);
                    if (!m.writeLayer(*m.held, m.firstGeometryFrame, true, error)) return false;
                    m.held.reset();
                }
                set.frames.push_back(m.firstGeometryFrame);
            }
            m.sampleGeometry(layer, f, *geometry);
            set.frames.push_back(f);
            named = true;
            growDrawn(m.scene, *geometry);
            m.lastGeometry = geometry;
        }
    }

    // The bodies: a pose each; the grit into the layer.
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
        const std::vector<uint8_t> whole = wholePanes(r);
        for (size_t b = 0; b < m.cracked.size(); ++b) {
            if (b >= whole.size() || !whole[b]) m.cracked[b] = std::min(m.cracked[b], f);
        }
        if (!r.debris.empty()) {
            m.sampleGrit(layer, f, r);
            Impl::ClipSet& set = m.clipSets["/World/grit"];
            set.frames.push_back(f);
            set.present.push_back(f);
            named = true;
        }
        if (r.rebar && m.sampleRebar(layer, f, r)) {
            Impl::ClipSet& set = m.clipSets["/World/rebar"];
            set.frames.push_back(f);
            set.present.push_back(f);
            named = true;
        }
    }

    // The water's surface -- unless the look hides it.
    if (!frame.water.empty() && look.waterSurface) {
        const std::shared_ptr<Geometry> surface = waterMesh(frame.water, &frame.rain);
        if (surface->primitiveCount() > 0) {
            m.sampleWater(layer, f, *surface);
            Impl::ClipSet& set = m.clipSets["/World/water"];
            set.frames.push_back(f);
            set.present.push_back(f);
            named = true;
            const Domain d = frame.water.flowDomain();
            usda::Bounds box;
            box.grow(d.origin());
            box.grow(d.origin() + d.size());
            m.scene.grow(box);
        }
    }

    // The cloth -- when it is drawn.
    if (!frame.cloth.empty() && look.cloth && m.sampleCloth(layer, f, frame.cloth, look.clothColor)) {
        Impl::ClipSet& set = m.clipSets["/World/cloth"];
        set.frames.push_back(f);
        set.present.push_back(f);
        named = true;
    }

    // The grains -- when they are drawn.
    if (!frame.grains.empty() && frame.grains.fits() && look.grains) {
        m.sampleGrains(layer, f, frame.grains, look.grainColor);
        Impl::ClipSet& set = m.clipSets["/World/grains"];
        set.frames.push_back(f);
        set.present.push_back(f);
        named = true;
    }

    // The rain.
    if (raining(frame.rain)) {
        m.sampleRain(layer, f, frame.rain);
        Impl::ClipSet& set = m.clipSets["/World/rain"];
        set.frames.push_back(f);
        set.present.push_back(f);
        named = true;
    }

    // The gas: a file of it.
    const Domain& d = frame.domain;
    const size_t cells = d.cellCount();
    std::vector<uint16_t> scratch;
    const std::vector<uint16_t>& gas = frame.fields.empty() ? frame.fields : frame.denseFields(scratch);
    if (!gas.empty() && gas.size() >= 3 * cells) {
        const char* names[3] = {"density", "temperature", "flame"};
        std::vector<Volume> volumes;
        for (int channel = 0; channel < 3; ++channel) {
            std::vector<float> values(cells);
            for (size_t c = 0; c < cells; ++c) values[c] = fromHalf(gas[3 * c + static_cast<size_t>(channel)]);
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

    // The layer: out now, or held back with the geometry.
    if (hold) {
        m.held = std::make_unique<usda::Stage>(std::move(layer));
        return true;
    }
    return m.writeLayer(layer, f, named, error);
}

usda::Stage UsdExport::stage() const {
    const Impl& m = *impl_;
    usda::Stage s;
    const std::string rate = rateOf(m.fps);
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
    const Look look = m.looks.empty() ? Look() : m.looks.front().second;

    // The materials: the colour of displayColor, a little rough; the
    // water's, clear and smooth, bending light as water does; the rain's.
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
        auto plain = [&](const char* name, const Vec3& tint, float opacity, float roughness, float ior) {
            Prim& mat = looks.child("Material", name);
            mat.set("token", "outputs:surface.connect", std::string("</World/Looks/") + name + "/shader.outputs:surface>");
            Prim& sh = mat.child("Shader", "shader");
            sh.setUniform("token", "info:id", usda::quoted("UsdPreviewSurface"));
            sh.set("color3f", "inputs:diffuseColor", usda::tuple(tint));
            sh.set("float", "inputs:ior", usda::number(ior));
            sh.set("float", "inputs:opacity", usda::number(opacity));
            sh.set("float", "inputs:roughness", usda::number(roughness));
            sh.set("token", "outputs:surface", "");
        };
        if (m.clipSets.count("/World/water")) plain("water", look.waterColor, 0.35f, 0.02f, 1.33f);
        if (m.clipSets.count("/World/rain")) plain("rain", look.rainColor, look.rainOpacity, 0.05f, 1.33f);
        // Glass: clear and smooth, bending light as glass does, of the tint
        // its faces have.
        if (m.drawn && m.drawn->primitives().find("glass")) {
            Prim& glass = looks.child("Material", "glass");
            glass.set("token", "outputs:surface.connect", "</World/Looks/glass/shader.outputs:surface>");
            Prim& sh = glass.child("Shader", "shader");
            sh.setUniform("token", "info:id", usda::quoted("UsdPreviewSurface"));
            sh.set("color3f", "inputs:diffuseColor.connect", "</World/Looks/surface/color.outputs:result>");
            sh.set("float", "inputs:ior", "1.5");
            sh.set("float", "inputs:opacity", "0.1");
            sh.set("float", "inputs:roughness", "0.02");
            sh.set("token", "outputs:surface", "");
        }
    }

    // The displayed geometry: once, when it never changes; else from the
    // layers of the frames it changed at.
    if (m.firstGeometry) {
        Prim* g = nullptr;
        if (m.geometryChanges) {
            g = &m.clippedPrim(world, m.geometryPath(), "Xform", m.geometryPresent);
        } else {
            g = &world.children.emplace_back(
                usda::geometryPrim(m.geometryName, {{m.firstGeometryFrame, m.firstGeometry.get()}}, m.geometryPath()));
            m.visibility(*g, m.geometryPresent);
        }
        g->metadata.push_back(kBinding);
        g->relate("material:binding", kSurface);
    }

    // The bodies: a shape each, moved and turned.
    if (m.layout && !m.bodyFrames.empty()) {
        Prim& pieces = world.child("Xform", "pieces");
        pieces.metadata.push_back(kBinding);
        pieces.relate("material:binding", kSurface);
        const Group* inside = m.drawn->findGroup(m.insideGroup);
        if (inside && inside->classOf() != AttrClass::Primitive) inside = nullptr;
        const AttributeArray* glass = m.drawn->primitives().find("glass");
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
            // Its faces; the cracks of glass -- faces whose glass is 2 -- apart,
            // there once its pane has broken.
            std::vector<uint32_t> faces, cracks;
            std::vector<int32_t> glassy;  // the faces of glass, numbered as the Mesh has them
            int32_t shown = 0;  // the faces the Mesh has: closed, of three corners or more (meshText)
            for (const uint32_t p : m.layout->prims[b]) {
                const float kind = glassOf(glass, p);
                if (kind >= 1.5f) {
                    cracks.push_back(p);
                    continue;
                }
                faces.push_back(p);
                if (!m.drawn->primitiveClosed(p) || m.drawn->primitivePoints(p).size() < 3) continue;
                if (kind >= 0.5f) glassy.push_back(shown);
                ++shown;
            }
            usda::MeshText shape = usda::meshText(*m.drawn, faces, m.middles[b], inside, local);
            shape.primvars.clear();  // the shape at rest: nothing of it moves
            shape.velocities.clear();
            // What is glass is glass, cut or not.
            std::erase_if(shape.inside, [&](int32_t i) { return std::binary_search(glassy.begin(), glassy.end(), i); });
            Prim mesh = usda::meshPrim("mesh", {{m.first, shape}});
            if (!shape.inside.empty() || !glassy.empty()) {
                mesh.setUniform("token", "subsetFamily:materialBind:familyType", usda::quoted("nonOverlapping"));
            }
            if (!shape.inside.empty()) {
                Prim& cut = mesh.child("GeomSubset", "inside");
                cut.setUniform("token", "elementType", usda::quoted("face"));
                cut.setUniform("token", "familyName", usda::quoted("materialBind"));
                cut.set("int[]", "indices", usda::integers(shape.inside));
            }
            if (!glassy.empty()) {
                Prim& clear = mesh.child("GeomSubset", "glass");
                clear.metadata.push_back(kBinding);
                clear.setUniform("token", "elementType", usda::quoted("face"));
                clear.setUniform("token", "familyName", usda::quoted("materialBind"));
                clear.set("int[]", "indices", usda::integers(glassy));
                clear.relate("material:binding", kGlass);
            }
            body.children.push_back(std::move(mesh));
            if (!cracks.empty()) {
                usda::MeshText lines = usda::meshText(*m.drawn, cracks, m.middles[b], nullptr, local);
                lines.primvars.clear();
                lines.velocities.clear();
                Prim crack = usda::meshPrim("cracks", {{m.first, lines}});
                crack.metadata.push_back(kBinding);
                crack.relate("material:binding", kGlass);
                if (m.cracked[b] == INT_MAX) {
                    crack.set("token", "visibility", usda::quoted("invisible"));
                } else if (m.cracked[b] > m.first) {
                    usda::Attribute& seen = crack.set("token", "visibility", "");
                    seen.samples = {{m.first, usda::quoted("invisible")}, {m.cracked[b], usda::quoted("inherited")}};
                }
                body.children.push_back(std::move(crack));
            }
        }
    }

    // The grit, the water and the rain: from the frames' layers.
    if (m.clipSets.count("/World/grit")) {
        Prim& grit = m.clippedPrim(world, "/World/grit", "Points", m.clipSets.at("/World/grit").present);
        grit.metadata.push_back(kBinding);
        grit.set("color3f[]", "primvars:displayColor", usda::tuples(std::span<const Vec3>(&m.gritColor, 1))).metadata =
            usda::interpolation("constant");
        grit.relate("material:binding", kSurface);
    }
    if (m.clipSets.count("/World/rebar")) {
        Prim& bars = m.clippedPrim(world, "/World/rebar", "BasisCurves", m.clipSets.at("/World/rebar").present);
        bars.metadata.push_back(kBinding);
        bars.set("color3f[]", "primvars:displayColor", usda::tuples(std::span<const Vec3>(&m.rebarColor, 1))).metadata =
            usda::interpolation("constant");
        bars.relate("material:binding", kSurface);
    }
    if (m.clipSets.count("/World/cloth")) {
        Prim& cloth = m.clippedPrim(world, "/World/cloth", "Xform", m.clipSets.at("/World/cloth").present);
        cloth.metadata.push_back(kBinding);
        cloth.relate("material:binding", kSurface);
    }
    if (m.clipSets.count("/World/grains")) {
        Prim& grains = m.clippedPrim(world, "/World/grains", "Points", m.clipSets.at("/World/grains").present);
        grains.metadata.push_back(kBinding);
        grains.relate("material:binding", kSurface);
    }
    if (m.clipSets.count("/World/water")) {
        Prim& water = m.clippedPrim(world, "/World/water", "Mesh", m.clipSets.at("/World/water").present);
        water.metadata.push_back(kBinding);
        water.set("color3f[]", "primvars:displayColor", usda::tuples(std::span<const Vec3>(&look.waterColor, 1))).metadata =
            usda::interpolation("constant");
        water.relate("material:binding", kWaterLook);
    }
    if (m.clipSets.count("/World/rain")) {
        Prim& rain = m.clippedPrim(world, "/World/rain", "Xform", m.clipSets.at("/World/rain").present);
        for (Prim& points : rain.children) {
            points.metadata.push_back(kBinding);
            points.set("color3f[]", "primvars:displayColor", usda::tuples(std::span<const Vec3>(&look.rainColor, 1))).metadata =
                usda::interpolation("constant");
            points.set("float[]", "widths", "[" + usda::number(points.name == "drops" ? kDropWidth : kDropletWidth) + "]")
                .metadata = usda::interpolation("constant");
            points.relate("material:binding", kRainLook);
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
        for (const auto& [f, k] : m.looks) exposure.emplace_back(f, usda::number(std::log2(std::max(k.exposure, 1e-6f))));
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
        for (const auto& [f, k] : m.looks) {
            sunColor.emplace_back(f, usda::tuple(k.lightColor));
            sunIntensity.emplace_back(f, usda::number(k.lightIntensity));
            near = Camera::rotationFor(k.lightDirection() * -1.0f, Vec3(0.0f, 1.0f, 0.0f), near);
            sunTurned.emplace_back(f, usda::tuple(near));
            skyColor.emplace_back(f, usda::tuple(k.skyColor));
            skyIntensity.emplace_back(f, usda::number(k.skyIntensity));
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

bool UsdExport::finish(std::string& error) {
    Impl& m = *impl_;
    if (m.frames == 0) {
        error = "no frame to write to " + m.path;
        return false;
    }
    std::error_code ec;
    const fs::path folder = fs::path(m.path).parent_path();
    if (!folder.empty()) fs::create_directories(folder, ec);
    // The layer held back: the geometry never changed, so it is not in it.
    if (m.held) {
        if (!m.writeLayer(*m.held, m.firstGeometryFrame, false, error)) return false;
        m.held.reset();
    }
    if (!m.clipped.empty()) {
        fs::create_directories(m.framesFolder, ec);
        const std::string manifest = (m.framesFolder / (m.stem + ".manifest.usda")).string();
        if (!usda::writeStage(m.manifest(), manifest, error)) return false;
    }
    return usda::writeStage(stage(), m.path, error);
}

}  // namespace pg::sim
