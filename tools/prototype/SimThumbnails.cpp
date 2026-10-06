// The thumbnails of the Simulation network's nodes (Thumbnails.h): what each
// node makes, drawn small by a renderer of their own beside the viewport's --
//
//   a geometry node        its geometry, as the cooker made it at the frame
//                          on screen
//   an object, a source    its shape, in its colour -- a source of fire
//                          orange, of smoke grey, of water blue
//   a solver, a look       what is simulated at the frame on screen -- the
//                          gas, the water, the pieces, the cloth, the
//                          grains, the rain -- in the scene's light
//   a camera, the Output   the scene through the camera; an Output without
//                          one, as the viewport frames it
//
// The forces have none. A picture of one thing is framed on it, seen from a
// little above, three quarters on, in a dark studio; the scene's pictures
// have its floor and its sky. The gas and the water go to the GPU on
// coarser grids than the viewport's when they are big.
//
#include "SimWorkspace.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pg::editor {
namespace {

using Clock = std::chrono::steady_clock;

/// At most this many pictures a frame of the window, and none more once
/// this many milliseconds went on them.
constexpr int kPerFrame = 3;
constexpr double kFrameMs = 8.0;
/// The most cells the thumbnails' gas and water each go to the GPU with.
constexpr size_t kThumbTexels = size_t(1) << 21;
constexpr float kPi = 3.14159265358979f;

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

/// An orbit that frames the box lo..hi: from a little above, three quarters on.
gl::Orbit framing(const Vec3& lo, const Vec3& hi) {
    gl::Orbit o;
    o.yaw = 35.0f;
    o.pitch = 24.0f;
    o.fovY = gl::VolumeRenderer::kFovY;
    o.target = (lo + hi) * 0.5f;
    // The sphere round the box a little taller than the picture: the box
    // itself, seen three quarters on, fills it.
    const float radius = std::max(0.5f * length(hi - lo), 0.02f);
    o.distance = 0.86f * radius / std::sin(o.fovY * kPi / 360.0f);
    return o;
}

/// The light of a picture of one thing: the sun high on the left, a dark
/// studio round it, no floor.
sim::Look studio() {
    sim::Look k;
    k.floor = false;
    k.grid = false;
    k.skyBehind = false;
    k.lightAzimuth = 135.0f;
    k.lightElevation = 50.0f;
    k.skyIntensity = 0.45f;
    return k;
}

/// What a thumbnail shows, and the keys it is drawn again by.
struct Picture {
    uint64_t key = 1469598103934665603ull;  ///< what the node is
    uint64_t live = 0;                      ///< what it is at the frame on screen
    GeometryPtr geometry, pieces;
    /// The whole scene: its geometry as the viewport has it prepared.
    bool scene = false;
    std::shared_ptr<const sim::PreparedGeometry> prepared;
    std::vector<sim::Solid> solids;
    std::shared_ptr<const sim::Frame> frame;
    unsigned layers = 0;
    sim::Look look = studio();
    bool hasOrbit = false;  ///< through a camera; else framed on what is drawn
    gl::Orbit orbit;
    bool hasBox = false;    ///< what the frame shows lies in lo..hi
    Vec3 lo, hi;

    void box(const Vec3& a, const Vec3& b) {
        lo = hasBox ? glm::min(lo, a) : a;
        hi = hasBox ? glm::max(hi, b) : b;
        hasBox = true;
    }
};

uint64_t keyOf(uint64_t key, const sim::Solid& s) {
    const sim::Collider& b = s.body;
    key = mixKey(key, b.shape);
    key = mixKey(key, b.center);
    key = mixKey(key, b.rotation);
    key = mixKey(key, b.size);
    key = mixKey(key, b.mesh.get());
    key = mixKey(key, s.color);
    return key;
}

/// A source drawn as an object of its shape.
template <class Source>
sim::Solid solidOf(const Source& source, const Vec3& color) {
    sim::Solid s;
    s.body.shape = source.shape;
    s.body.center = source.center;
    s.body.rotation = source.rotation;
    s.body.size = source.size;
    s.body.mesh = source.mesh;
    s.body.node = source.node;
    s.color = color;
    return s;
}

uint64_t keyOf(uint64_t key, const sim::Camera& c) {
    key = mixKey(key, c.position);
    key = mixKey(key, c.rotation);
    key = mixKey(key, c.focal);
    key = mixKey(key, c.width);
    return mixKey(key, c.height);
}

/// Where the gas of a frame is: its tiles' box, or the cells with smoke.
bool gasBox(const sim::Frame& f, Vec3& lo, Vec3& hi) {
    const sim::Domain& d = f.domain;
    int a[3] = {d.cells[0], d.cells[1], d.cells[2]}, b[3] = {-1, -1, -1};
    auto take = [&](int i, int j, int k) {
        const int c[3] = {i, j, k};
        for (int x = 0; x < 3; ++x) {
            a[x] = std::min(a[x], c[x]);
            b[x] = std::max(b[x], c[x]);
        }
    };
    if (!f.gasTiles.empty()) {
        const int t[3] = {(d.cells[0] + 7) / 8, (d.cells[1] + 7) / 8, (d.cells[2] + 7) / 8};
        for (const uint32_t tile : f.gasTiles) {
            const int i = static_cast<int>(tile % static_cast<uint32_t>(t[0])) * 8;
            const int j = static_cast<int>((tile / static_cast<uint32_t>(t[0])) % static_cast<uint32_t>(t[1])) * 8;
            const int k = static_cast<int>(tile / static_cast<uint32_t>(t[0] * t[1])) * 8;
            take(i, j, k);
            take(std::min(i + 7, d.cells[0] - 1), std::min(j + 7, d.cells[1] - 1), std::min(k + 7, d.cells[2] - 1));
        }
    } else if (f.fields.size() == 3 * d.cellCount()) {
        // Smoke or flame above a hundredth: a positive half is ordered as its bits are.
        const uint16_t some = sim::toHalf(0.01f);
        size_t c = 0;
        for (int k = 0; k < d.cells[2]; ++k) {
            for (int j = 0; j < d.cells[1]; ++j) {
                for (int i = 0; i < d.cells[0]; ++i, ++c) {
                    const uint16_t smoke = f.fields[3 * c], flame = f.fields[3 * c + 2];
                    if ((smoke < 0x8000 && smoke > some) || (flame < 0x8000 && flame > some)) take(i, j, k);
                }
            }
        }
    }
    if (b[0] < 0) return false;
    const Vec3 o = d.origin();
    lo = o + Vec3(static_cast<float>(a[0]), static_cast<float>(a[1]), static_cast<float>(a[2])) * d.voxel;
    hi = o + Vec3(static_cast<float>(b[0] + 1), static_cast<float>(b[1] + 1), static_cast<float>(b[2] + 1)) * d.voxel;
    return true;
}

/// Where the drops of a frame's rain are.
bool rainBox(const sim::RainFrame& rain, Vec3& lo, Vec3& hi) {
    bool any = false;
    for (size_t i = 0; i + 5 < rain.drops.size(); i += 6) {
        const Vec3 p(rain.drops[i], rain.drops[i + 1], rain.drops[i + 2]);
        lo = any ? glm::min(lo, p) : p;
        hi = any ? glm::max(hi, p) : p;
        any = true;
    }
    return any;
}

}  // namespace

SimWorkspace::ThumbKind SimWorkspace::thumbKindOf(const sim::Node& n) {
    const sim::NodeType* t = sim::findNodeType(n.type);
    if (!t) return ThumbKind::None;
    if (t->core) return ThumbKind::Geometry;
    const std::string& y = n.type;
    if (y == "object") return ThumbKind::Object;
    if (y == "pyro_source" || y == "water_source") return ThumbKind::Source;
    if (y == "pyro_solver" || y == "pyro_upres" || y == "vdb_gas" || y == "volume_look") return ThumbKind::Gas;
    if (y == "liquid_solver" || y == "water_look") return ThumbKind::Water;
    if (y == "rbd_solver") return ThumbKind::Pieces;
    if (y == "cloth_solver") return ThumbKind::Cloth;
    if (y == "grain_solver") return ThumbKind::Grains;
    if (y == "rain") return ThumbKind::Rain;
    if (y == "camera" || y == "usd_camera" || y == "alembic_camera") return ThumbKind::Camera;
    if (y == "output") return ThumbKind::Output;
    return ThumbKind::None;  // the forces
}

bool SimWorkspace::showsThumbnail(const sim::Node& n) const {
    return thumbnails_ && !thumbnailsHidden_.count(n.id) && thumbKindOf(n) != ThumbKind::None;
}

std::vector<int> SimWorkspace::thumbnailGeometryWanted() const {
    std::vector<int> out;
    if (!thumbnails_) return out;
    for (const int id : canvas_.thumbnailsShown()) {
        const sim::Node* n = net_.node(id);
        if (n && showsThumbnail(*n) && thumbKindOf(*n) == ThumbKind::Geometry && geometry_->contains(id)) {
            out.push_back(id);
        }
    }
    std::sort(out.begin(), out.end());  // the same nodes: the same request, whatever is selected
    return out;
}

void SimWorkspace::noteThumbnailGeometry(int node, const GeometryPtr& geometry) {
    ThumbGeometry& t = thumbGeometry_[node];
    if (t.stamp && t.geometry == geometry) return;
    t.geometry = geometry;
    t.stamp = ++thumbStamp_;
    t.revision = net_.revision();
}

void SimWorkspace::clearThumbnails() {
    if (thumbs_) thumbs_->clear();
    thumbGeometry_.clear();
    thumbCookKey_.clear();
}

void SimWorkspace::updateThumbnails() {
    // The nodes gone: their pictures, and the geometry kept for them, too.
    std::set<int> alive;
    for (const sim::Node& n : net_.nodes()) alive.insert(n.id);
    for (auto it = thumbGeometry_.begin(); it != thumbGeometry_.end();) {
        it = alive.count(it->first) ? std::next(it) : thumbGeometry_.erase(it);
    }
    if (thumbs_) thumbs_->keep(alive);
    const std::vector<int> shown = canvas_.thumbnailsShown();
    if (!thumbnails_ || shown.empty() || !rendererLog_.empty()) return;
    if (!thumbRenderer_) {
        if (!thumbRendererLog_.empty()) return;  // tried, and it would not
        auto made = [&]() -> std::unique_ptr<gl::VolumeRenderer> {
            auto r = std::make_unique<gl::VolumeRenderer>(gl_);
            if (!r->init(thumbRendererLog_)) {
                if (thumbRendererLog_.empty()) thumbRendererLog_ = "the thumbnails' renderer did not start";
                return nullptr;
            }
            r->texelBudget = kThumbTexels;
            return r;
        };
        auto r = made();
        auto scene = r ? made() : nullptr;
        if (!scene) return;
        thumbRenderer_ = std::move(r);
        sceneThumbRenderer_ = std::move(scene);
        thumbs_ = std::make_unique<Thumbnails>(gl_);
    }

    const bool scene = levels_.empty();  // inside an asset: its geometry alone
    const std::shared_ptr<const sim::Frame> frame = scene ? shown_ : nullptr;
    const uint64_t revision = mixKey(1469598103934665603ull, compiledRevision_);
    const uint64_t live = frame ? mixKey(mixKey(1469598103934665603ull, frame.get()), frame->number) : 0;
    auto sceneLook = [&] {
        sim::Look k = compiled_.lookAt(current_);
        k.grid = false;
        return k;
    };
    // The whole scene, as the Output draws it -- through `camera`, if one.
    // Made in `full` only for the picture to be drawn: the rest want its keys.
    auto wholeScene = [&](Picture& p, const sim::Camera* camera, bool full) {
        p.look = sceneLook();
        p.frame = frame;
        p.layers = gl::VolumeRenderer::kAllLayers;
        p.solids = compiled_.solidsAt(current_);
        p.geometry = renderer_.geometry();
        p.scene = true;
        p.prepared = renderer_.prepared();
        if (frame && full) p.pieces = sim::drawnBodies(*frame, p.look);
        p.key = mixKey(revision, p.geometry.get());
        p.live = live;
        if (camera) {
            p.hasOrbit = true;
            p.orbit = gl::orbitThrough(*camera, focusOf(*camera));
            p.key = keyOf(p.key, *camera);
        } else {
            p.hasOrbit = true;
            p.orbit = gl::VolumeRenderer::viewOf(sceneBox());
        }
    };
    // What node `n`'s thumbnail shows; false while there is nothing. Not
    // `full`: its keys, and what is cheap.
    auto pictureOf = [&](const sim::Node& n, Picture& p, bool full) -> bool {
        const int id = n.id;
        switch (thumbKindOf(n)) {
            case ThumbKind::None:
                return false;
            case ThumbKind::Geometry: {
                const auto g = thumbGeometry_.find(id);
                if (g == thumbGeometry_.end() || !g->second.geometry) return false;
                p.geometry = g->second.geometry;
                // Made by an edit: at once; by the frame playing on: as often as the pictures may.
                p.key = mixKey(p.key, g->second.revision);
                p.live = g->second.stamp;
                return true;
            }
            case ThumbKind::Object: {
                if (!scene) return false;
                for (const sim::Solid& s : compiled_.solidsAt(current_)) {
                    if (s.body.node != id) continue;
                    p.solids = {s};
                    p.key = keyOf(p.key, s);
                    return true;
                }
                return false;
            }
            case ThumbKind::Source: {
                if (!scene) return false;
                sim::Solid s;
                bool found = false;
                for (const sim::Emitter& e : compiled_.world.gas.emitters) {
                    if (e.node != id || found) continue;
                    s = solidOf(e, e.fuel > 0.0f ? Vec3(0.95f, 0.42f, 0.12f) : Vec3(0.62f, 0.62f, 0.64f));
                    found = true;
                }
                for (const sim::WaterSource& w : compiled_.world.water.sources) {
                    if (w.node != id || found) continue;
                    s = solidOf(w, Vec3(0.16f, 0.46f, 0.86f));
                    found = true;
                }
                if (!found) return false;
                p.solids = {s};
                p.key = keyOf(p.key, s);
                return true;
            }
            case ThumbKind::Gas: {
                if (!frame || frame->fields.empty() ||
                    (id != compiled_.solver && id != compiled_.upres && id != compiled_.lookNode)) {
                    return false;
                }
                p.frame = frame;
                p.layers = gl::VolumeRenderer::kGas;
                p.look = sceneLook();
                if (full) {
                    Vec3 lo, hi;
                    if (gasBox(*frame, lo, hi)) p.box(lo, hi);
                    else p.box(frame->domain.origin(), frame->domain.origin() + frame->domain.size());
                }
                p.key = revision;
                p.live = live;
                return true;
            }
            case ThumbKind::Water: {
                if (!frame || frame->water.empty() || (id != compiled_.liquidSolver && id != compiled_.waterLook)) return false;
                p.frame = frame;
                p.layers = gl::VolumeRenderer::kWater;
                p.look = sceneLook();
                const sim::Domain& d = frame->water.domain;
                p.box(d.origin(), d.origin() + d.size());
                p.key = revision;
                p.live = live;
                return true;
            }
            case ThumbKind::Pieces:
            case ThumbKind::Cloth:
            case ThumbKind::Grains: {
                const ThumbKind kind = thumbKindOf(n);
                const int solver = kind == ThumbKind::Pieces  ? compiled_.rigid
                                   : kind == ThumbKind::Cloth ? compiled_.cloth
                                                              : compiled_.grains;
                if (!frame || id != solver) return false;
                const bool empty = kind == ThumbKind::Pieces  ? frame->rigid.empty()
                                   : kind == ThumbKind::Cloth ? frame->cloth.empty()
                                                              : frame->grains.empty();
                if (empty) return false;
                sim::Look k = sceneLook();
                k.pieces = kind == ThumbKind::Pieces;
                k.cloth = kind == ThumbKind::Cloth;
                k.grains = kind == ThumbKind::Grains;
                if (full) p.pieces = sim::drawnBodies(*frame, k);
                p.look = k;
                p.key = revision;
                p.live = live;
                return true;
            }
            case ThumbKind::Rain: {
                if (!frame || frame->rain.drops.size() < 6 || id != compiled_.rain) return false;
                p.frame = frame;
                p.layers = gl::VolumeRenderer::kRain;
                p.look = sceneLook();
                Vec3 lo, hi;
                if (full && rainBox(frame->rain, lo, hi)) p.box(lo, hi);
                p.key = revision;
                p.live = live;
                return true;
            }
            case ThumbKind::Camera: {
                if (!scene) return false;
                sim::Camera c;
                if (compiled_.hasCamera && compiled_.camera.node == id) {
                    c = compiled_.cameraAt(current_);
                } else if (n.type == "camera") {
                    // A camera the Output does not look through: as it would.
                    const sim::ParamValue center = net_.param(id, "center"), rotation = net_.param(id, "rotation");
                    c.position = Vec3(center[0], center[1], center[2]);
                    c.rotation = Vec3(rotation[0], rotation[1], rotation[2]);
                    c.focal = net_.value(id, "focal");
                    c.width = static_cast<int>(net_.value(id, "width"));
                    c.height = static_cast<int>(net_.value(id, "height"));
                    c = c.sanitized();
                } else {
                    return false;  // a USD Camera's camera is read for the Output's alone
                }
                wholeScene(p, &c, full);
                return true;
            }
            case ThumbKind::Output: {
                if (!scene || !compiled_.ok) return false;
                const sim::Camera camera = compiled_.cameraAt(current_);
                wholeScene(p, compiled_.hasCamera ? &camera : nullptr, full);
                return true;
            }
        }
        return false;
    };
    // Draws `p` into the thumbnails' renderer -- the whole scene into its
    // own -- at twice the picture's size; the renderer it drew in.
    auto draw = [&](const Picture& p) -> gl::VolumeRenderer& {
        gl::VolumeRenderer& r = p.scene ? *sceneThumbRenderer_ : *thumbRenderer_;
        r.look = p.look;
        if (p.prepared && p.prepared->geometry == p.geometry) r.setPrepared(p.prepared);
        else r.setGeometry(p.geometry);
        r.setPieces(p.pieces);
        r.setSolids(p.solids);
        if (p.frame) r.setFrame(*p.frame, p.layers);
        else r.clearFrame();
        if (p.hasOrbit) {
            r.orbit = p.orbit;
        } else {
            Picture framed = p;
            Vec3 lo, hi;
            if (r.geometryBounds(lo, hi)) framed.box(lo, hi);
            for (const sim::Solid& s : p.solids) {
                s.body.instance().bounds(lo, hi);
                framed.box(lo, hi);
            }
            r.orbit = framed.hasBox ? framing(framed.lo, framed.hi) : gl::Orbit();
        }
        r.render(2 * Thumbnails::kWidth, 2 * Thumbnails::kHeight);
        return r;
    };

    // The pictures due, the most wanted first: those with none, those an
    // edit changed, then those the frame playing on did -- the longest
    // waiting first; among equals, the selected and the topmost first.
    const double now = ImGui::GetTime();
    struct Due {
        int urgency;
        double drawn;
        size_t order;
        int node;
    };
    std::vector<Due> due;
    for (size_t i = 0; i < shown.size(); ++i) {
        const sim::Node* n = net_.node(shown[i]);
        if (!n || !showsThumbnail(*n)) continue;
        Picture p;
        if (!pictureOf(*n, p, false)) continue;
        if (const int urgency = thumbs_->stale(n->id, p.key, p.live, now)) {
            due.push_back({urgency, thumbs_->drawnAt(n->id), i, n->id});
        }
    }
    std::sort(due.begin(), due.end(), [](const Due& a, const Due& b) {
        if (a.urgency != b.urgency) return a.urgency < b.urgency;
        if (a.drawn != b.drawn) return a.drawn < b.drawn;
        return a.order < b.order;
    });
    const auto start = Clock::now();
    int drawn = 0;
    for (const Due& d : due) {
        if (drawn >= kPerFrame || (drawn > 0 && msSince(start) > kFrameMs)) break;
        Picture p;
        if (!pictureOf(*net_.node(d.node), p, true)) continue;
        const auto t0 = Clock::now();
        const gl::VolumeRenderer& r = draw(p);
        thumbs_->take(d.node, r.colorTexture(), p.key, p.live, now, msSince(t0));
        ++drawn;
    }
}

}  // namespace pg::editor
