// The Simulation network's viewport: the picture, and what the mouse and the
// keys do in it, as in a 3D program --
//
//   click                  select what is under the mouse (Shift, Ctrl: add)
//   drag a handle          move, turn or size the selection (Gizmo.h)
//   left drag elsewhere    orbit;  middle or Shift + left drag: pan;
//   right drag, wheel      zoom;   right click: the menu
//   Q W E R                select, move, rotate, scale
//   Shift+A                add an object, a source, a force
//   Delete / X, Ctrl+D     delete, duplicate the selection
//   F                      frame the selection;  double click: frame what is clicked
//   Escape                 undo the drag under way, or select nothing
//   2 3 4, P               pick points, edges, primitives; paint (SimElements.cpp) -- 1: objects
//
// A drag starts from the values the nodes had when it began (Placed), so the
// gizmo's rounding never builds up; the History records it once let go.
// While a gizmo is dragged the simulation waits (SimRunner::hold): it starts
// again from the scene that is let go.
#include "SimWorkspace.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace pg::editor {
namespace {

using theme::Icon;

Vec3 v3(const sim::ParamValue& p) { return {p[0], p[1], p[2]}; }
sim::ParamValue pv(const Vec3& v) { return {v.x, v.y, v.z}; }

/// A turn whose y axis is `axis`: the frame of a vortex, of the wind.
sim::Rotation alongY(const Vec3& axis) {
    const Vec3 y = normalize(axis);
    if (length(y) < 0.5f) return sim::Rotation();
    const Vec3 helper = std::fabs(y.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);
    sim::Rotation r;
    r.y = y;
    r.z = normalize(cross(helper, y));
    r.x = cross(r.y, r.z);
    return r;
}

/// Colours new objects take, in turn.
const Vec3 kPalette[] = {{0.72f, 0.36f, 0.27f}, {0.32f, 0.5f, 0.72f}, {0.44f, 0.6f, 0.36f},
                         {0.76f, 0.63f, 0.32f}, {0.56f, 0.44f, 0.68f}, {0.52f, 0.52f, 0.54f}};

/// A menu item with an icon in front.
bool iconItem(Icon icon, ImU32 color, const char* label, const char* shortcut = nullptr) {
    return ui::iconMenuItem(icon, color, label, shortcut);
}

/// What objects and forces are linked into: the solvers, and the rain.
bool isSolver(const std::string& type) { return type == "pyro_solver" || type == "liquid_solver" || type == "rain"; }

}  // namespace

// --- what the gizmo moves ------------------------------------------------------------

bool SimWorkspace::placeOf(int id, Vec3& center, sim::Rotation& frame) const {
    const sim::Node* n = net_.node(id);
    const sim::NodeType* t = n ? sim::findNodeType(n->type) : nullptr;
    if (!t || !t->handles.any()) return false;
    const sim::Handles& h = t->handles;
    if (h.center) {
        center = v3(net_.valueAt(id, h.center, static_cast<float>(current_)));
        if (h.pivot) center += v3(net_.valueAt(id, h.pivot, static_cast<float>(current_)));
    } else {
        // The wind blows everywhere: its handle is where its arrows are drawn,
        // over the middle of the domain.
        center = Vec3(0.0f, 0.6f * sceneBox().size().y, 0.0f);
    }
    frame = h.rotation ? sim::Rotation::fromEuler(v3(net_.valueAt(id, h.rotation, static_cast<float>(current_))))
            : h.axis   ? alongY(v3(net_.valueAt(id, h.axis, static_cast<float>(current_))))
                       : sim::Rotation();
    return true;
}

std::vector<int> SimWorkspace::movable() const {
    std::vector<int> out;
    for (const int id : canvas_.selection()) {
        const sim::Node* n = net_.node(id);
        const sim::NodeType* t = n ? sim::findNodeType(n->type) : nullptr;
        // Shaped by geometry: its own place is not used -- the geometry's nodes move it.
        if (t && t->input("shape") && !net_.linksInto(id, "shape").empty()) continue;
        if (t && t->handles.any()) out.push_back(id);
    }
    return out;
}

GizmoMode SimWorkspace::toolFor(const std::vector<int>& nodes) const {
    bool move = false, rotate = false, scale = false;
    for (const int id : nodes) {
        const sim::Handles& h = sim::findNodeType(net_.node(id)->type)->handles;
        move = move || h.center;
        rotate = rotate || h.rotation || h.axis;
        scale = scale || h.size || h.radius || h.height;
    }
    auto can = [&](GizmoMode m) {
        return m == GizmoMode::Move ? move : m == GizmoMode::Rotate ? rotate : m == GizmoMode::Scale ? scale : true;
    };
    if (can(tool_)) return tool_;
    for (const GizmoMode m : {GizmoMode::Move, GizmoMode::Rotate, GizmoMode::Scale}) {
        if (can(m)) return m;
    }
    return GizmoMode::Select;
}

void SimWorkspace::pivotOf(const std::vector<int>& nodes, GizmoMode tool, Vec3& pivot, sim::Rotation& frame) const {
    Vec3 sum;
    int count = 0;
    for (const int id : nodes) {
        Vec3 c;
        sim::Rotation f;
        if (!placeOf(id, c, f)) continue;
        sum += c;
        ++count;
    }
    pivot = count ? sum * (1.0f / static_cast<float>(count)) : Vec3();
    frame = sim::Rotation();
    // Sizes are along a node's own axes: scaling is always in them.
    if (localAxes_ || tool == GizmoMode::Scale) {
        const int current = std::find(nodes.begin(), nodes.end(), canvas_.current()) != nodes.end() ? canvas_.current()
                                                                                                   : nodes.front();
        Vec3 c;
        placeOf(current, c, frame);
    }
}

SimWorkspace::Placed SimWorkspace::placedOf(int id) const {
    Placed p;
    p.id = id;
    const sim::Handles& h = sim::findNodeType(net_.node(id)->type)->handles;
    if (h.center) p.center = net_.valueAt(id, h.center, static_cast<float>(current_));
    if (h.rotation) p.rotation = net_.valueAt(id, h.rotation, static_cast<float>(current_));
    if (h.axis) p.axis = net_.valueAt(id, h.axis, static_cast<float>(current_));
    if (h.size) p.size = net_.valueAt(id, h.size, static_cast<float>(current_));
    if (h.radius) p.radius = net_.valueAt(id, h.radius, static_cast<float>(current_))[0];
    if (h.height) p.height = net_.valueAt(id, h.height, static_cast<float>(current_))[0];
    return p;
}

void SimWorkspace::applyDrag(const GizmoDrag& drag) {
    const sim::Rotation turn = sim::Rotation::about(drag.axis, drag.degrees);
    const bool several = dragStart_.size() > 1;
    // The factor that says most: the one furthest from 1.
    auto strongest = [](float a, float b) { return std::fabs(std::log(a)) >= std::fabs(std::log(b)) ? a : b; };
    for (const Placed& p : dragStart_) {
        const sim::Node* n = net_.node(p.id);
        if (!n) continue;
        const sim::Handles& h = sim::findNodeType(n->type)->handles;
        switch (drag.mode) {
            case GizmoMode::Select: break;
            case GizmoMode::Move:
                if (h.center) net_.setParamAt(p.id, h.center, static_cast<float>(current_), pv(v3(p.center) + drag.move));
                break;
            case GizmoMode::Rotate:
                if (h.rotation) {
                    const Vec3 before = v3(p.rotation);
                    net_.setParamAt(p.id, h.rotation, static_cast<float>(current_), pv(turn.then(sim::Rotation::fromEuler(before)).toEuler(before)));
                }
                if (h.axis) net_.setParamAt(p.id, h.axis, static_cast<float>(current_), pv(turn.apply(v3(p.axis))));
                // Several turn together, round their middle: each where its handle is.
                if (h.center && several) {
                    const Vec3 off = h.pivot ? v3(net_.valueAt(p.id, h.pivot, static_cast<float>(current_))) : Vec3();
                    const Vec3 at = dragPivot_ + turn.apply(v3(p.center) + off - dragPivot_);
                    net_.setParamAt(p.id, h.center, static_cast<float>(current_), pv(at - off));
                }
                break;
            case GizmoMode::Scale: {
                const Vec3& s = drag.scale;
                if (h.size) net_.setParamAt(p.id, h.size, static_cast<float>(current_), pv(v3(p.size) * s));
                if (h.radius) {
                    const float f = h.axis ? strongest(s.x, s.z) : strongest(strongest(s.x, s.y), s.z);
                    net_.setParamAt(p.id, h.radius, static_cast<float>(current_), {p.radius * f, 0.0f, 0.0f});
                }
                if (h.height) net_.setParamAt(p.id, h.height, static_cast<float>(current_), {p.height * s.y, 0.0f, 0.0f});
                break;
            }
        }
    }
}

void SimWorkspace::restoreDrag() {
    for (const Placed& p : dragStart_) {
        const sim::Node* n = net_.node(p.id);
        if (!n) continue;
        const sim::Handles& h = sim::findNodeType(n->type)->handles;
        if (h.center) net_.setParamAt(p.id, h.center, static_cast<float>(current_), p.center);
        if (h.rotation) net_.setParamAt(p.id, h.rotation, static_cast<float>(current_), p.rotation);
        if (h.axis) net_.setParamAt(p.id, h.axis, static_cast<float>(current_), p.axis);
        if (h.size) net_.setParamAt(p.id, h.size, static_cast<float>(current_), p.size);
        if (h.radius) net_.setParamAt(p.id, h.radius, static_cast<float>(current_), {p.radius, 0.0f, 0.0f});
        if (h.height) net_.setParamAt(p.id, h.height, static_cast<float>(current_), {p.height, 0.0f, 0.0f});
    }
    dragStart_.clear();
}

void SimWorkspace::keySelection() {
    const float frame = static_cast<float>(current_);
    int keyed = 0;
    for (const int id : movable()) {
        const sim::Handles& h = sim::findNodeType(net_.node(id)->type)->handles;
        for (const char* name : {h.center, h.rotation, h.axis, h.size, h.radius, h.height}) {
            if (!name) continue;
            net_.setKey(id, name, frame, net_.valueAt(id, name, frame));
            ++keyed;
        }
    }
    if (keyed) setMessage("Keyed at frame " + std::to_string(current_) + ": move to another frame, move it, key again (K)");
    else setMessage("Select an object, a source or a force to key where it is", true);
}

// --- picking --------------------------------------------------------------------------

int SimWorkspace::pickAt(const ViewCamera& cam, ImVec2 mouse) const {
    Vec3 o, d;
    cam.ray(mouse, o, d);
    float best = 1e30f;
    int node = 0;
    // The objects and the sources, where the ray meets them.
    for (const sim::Solid& s : compiled_.solidsAt(current_)) {
        float t = 0.0f;
        Vec3 n;
        if (s.body.instance().intersect(o, d, 0.0f, t, n) && t < best) {
            best = t;
            node = s.body.node;
        }
    }
    if (compiled_.ok && compiled_.world.hasGas) {
        for (const sim::Emitter& e : compiled_.world.gas.emitters) {
            float t = 0.0f;
            Vec3 n;
            if (e.shapeAt(0.0f).intersect(o, d, 0.0f, t, n) && t < best) {
                best = t;
                node = e.node;
            }
        }
    }
    if (compiled_.ok && compiled_.world.hasWater) {
        for (const sim::WaterSource& w : compiled_.world.water.sources) {
            float t = 0.0f;
            Vec3 n;
            if (w.instance().intersect(o, d, 0.0f, t, n) && t < best) {
                best = t;
                node = w.node;
            }
        }
    }
    // The guides: a line near the mouse, unless it is behind what was met.
    const float reach = theme::px(6.0f);
    const auto& v = guideLines_.vertices;
    float nearest = reach;
    for (size_t i = 0; i + 1 < v.size() && i / 2 < guideLines_.owners.size(); i += 2) {
        const int owner = guideLines_.owners[i / 2];
        if (owner == 0) continue;
        const Vec3 a(v[i].position[0], v[i].position[1], v[i].position[2]);
        const Vec3 b(v[i + 1].position[0], v[i + 1].position[1], v[i + 1].position[2]);
        ImVec2 sa, sb;
        if (!cam.toScreen(a, sa) || !cam.toScreen(b, sb)) continue;
        float along = 0.0f;
        const float pixels = pixelsToSegment(mouse, sa, sb, &along);
        if (pixels > nearest) continue;
        const float t = length(a + (b - a) * along - o);
        // A line drawn on a surface is a hair in front of it.
        if (t <= best * 1.02f) {
            nearest = pixels;
            node = owner;
            best = std::min(best, t);
        }
    }
    return node;
}

Vec3 SimWorkspace::floorPoint(const ViewCamera& cam, ImVec2 screen) const {
    Vec3 o, d;
    cam.ray(screen, o, d);
    if (d.y < -1e-4f && o.y > 0.0f) {
        const float t = -o.y / d.y;
        if (t < 200.0f) {
            const Vec3 p = o + d * t;
            return Vec3(p.x, 0.0f, p.z);
        }
    }
    const gl::Orbit& orbit = renderer_.orbit;
    return Vec3(orbit.target[0], 0.0f, orbit.target[2]);
}

// --- adding to the scene -----------------------------------------------------------------

ImVec2 SimWorkspace::freeSlot() const {
    float x = 1e30f, y = -1e30f;
    bool any = false;
    for (const sim::Node& n : net_.nodes()) {
        const sim::NodeType* t = sim::findNodeType(n.type);
        if (!t) continue;
        const std::string c = t->category;
        if (c != "Objects" && c != "Sources" && c != "Forces") continue;
        x = std::min(x, n.x);
        y = std::max(y, n.y);
        any = true;
    }
    return any ? ImVec2(x, y + 96.0f) : ImVec2(0.0f, 0.0f);
}

void SimWorkspace::linkIntoSolvers(int node, const char* output, const char* input) {
    for (const sim::Node& n : std::vector<sim::Node>(net_.nodes())) {
        if (!isSolver(n.type)) continue;
        const sim::NodeType* t = sim::findNodeType(n.type);
        if (t && t->input(input)) net_.connect(node, output, n.id, input);
    }
}

int SimWorkspace::ensurePyroChain() {
    for (const sim::Node& n : net_.nodes()) {
        if (n.type == "pyro_solver") return n.id;
    }
    // Nothing simulates gas yet: a solver, its look and the output.
    const int solver = net_.add("pyro_solver", 250.0f, 40.0f);
    const int look = net_.add("volume_look", 490.0f, 40.0f);
    int output = 0;
    for (const sim::Node& n : net_.nodes()) {
        if (n.type == "output") output = n.id;
    }
    if (!output) output = net_.add("output", 720.0f, 40.0f);
    net_.connect(solver, "gas", look, "gas");
    net_.connect(look, "look", output, "look");  // one layer more of the picture
    // The objects already there are in its way.
    for (const sim::Node& n : std::vector<sim::Node>(net_.nodes())) {
        if (n.type == "object") net_.connect(n.id, "collider", solver, "colliders");
    }
    return solver;
}

int SimWorkspace::ensureLiquidChain() {
    for (const sim::Node& n : net_.nodes()) {
        if (n.type == "liquid_solver") return n.id;
    }
    // Nothing simulates water yet: a solver, its look, into the output --
    // below the gas's, if there is one.
    float y = 40.0f;
    for (const sim::Node& n : net_.nodes()) {
        if (n.type == "pyro_solver" || n.type == "volume_look") y = std::max(y, n.y + 200.0f);
    }
    const int solver = net_.add("liquid_solver", 250.0f, y);
    const int look = net_.add("water_look", 490.0f, y);
    int output = 0;
    for (const sim::Node& n : net_.nodes()) {
        if (n.type == "output") output = n.id;
    }
    if (!output) output = net_.add("output", 720.0f, y);
    net_.connect(solver, "liquid", look, "liquid");
    net_.connect(look, "look", output, "look");
    // The objects already there are in its way; the forces push it too.
    for (const sim::Node& n : std::vector<sim::Node>(net_.nodes())) {
        if (n.type == "object") net_.connect(n.id, "collider", solver, "colliders");
    }
    return solver;
}

int SimWorkspace::addRain(bool storm) {
    // A cloud over the whole scene, drawn as a layer of the Output, below
    // the looks already there.
    float y = 40.0f;
    int output = 0;
    for (const sim::Node& n : net_.nodes()) {
        const sim::NodeType* t = sim::findNodeType(n.type);
        if (t && (std::string(t->category) == "Simulation" || std::string(t->category) == "Render") &&
            n.type != "output") {
            y = std::max(y, n.y + 200.0f);
        }
        if (n.type == "output") output = n.id;
    }
    const int id = net_.add("rain", 490.0f, y);
    if (!output) output = net_.add("output", 720.0f, y);
    net_.connect(id, "look", output, "look");
    const sim::Domain box = sceneBox();
    const Vec3 middle = box.origin() + box.size() * 0.5f;
    const Vec3 size(std::max(3.0f, box.size().x + 1.0f), 0.5f, std::max(3.0f, box.size().z + 1.0f));
    net_.setParam(id, "center", pv(Vec3(middle.x, std::max(2.5f, box.size().y + 0.8f), middle.z)));
    net_.setParam(id, "size", pv(size));
    // It lands on every object, and the wind that blows the rest blows it.
    for (const sim::Node& n : std::vector<sim::Node>(net_.nodes())) {
        if (n.type == "object") net_.connect(n.id, "collider", id, "colliders");
        if (n.type == "wind") net_.connect(n.id, "force", id, "forces");
    }
    if (storm) {
        net_.setParam(id, "rate", {2500.0f, 0.0f, 0.0f});
        net_.setParam(id, "speed", {9.0f, 0.0f, 0.0f});
        net_.setParam(id, "splash", {4.0f, 0.0f, 0.0f});
        net_.setParam(id, "ripples", {1.5f, 0.0f, 0.0f});
        net_.setParam(id, "wet", {0.85f, 0.0f, 0.0f});
        net_.rename(id, net_.uniqueName("rainstorm"));
        // A storm blows: a gusting wind, if there is none yet -- into the
        // rain, and into the solvers.
        if (net_.linksInto(id, "forces").empty()) {
            const ImVec2 slot = freeSlot();
            const int wind = net_.add("wind", slot.x, slot.y);
            net_.setParam(wind, "direction", pv(normalize(Vec3(1.0f, 0.0f, 0.3f))));
            net_.setParam(wind, "speed", {4.0f, 0.0f, 0.0f});
            net_.setParam(wind, "gusts", {0.6f, 0.0f, 0.0f});
            linkIntoSolvers(wind, "force", "forces");
        }
    }
    return id;
}

int SimWorkspace::addToScene(const std::string& kind, const Vec3& at) {
    const ImVec2 slot = freeSlot();
    int id = 0;
    std::string what = kind;
    static const char* const shapes[] = {"sphere", "box", "cylinder", "cone", "torus"};
    for (int s = 0; s < 5; ++s) {
        if (kind != shapes[s]) continue;
        // An object resting on the floor where it is put, in a colour of its own.
        id = net_.add("object", slot.x, slot.y);
        net_.setParam(id, "shape", {static_cast<float>(s), 0.0f, 0.0f});
        const Vec3 size = v3(net_.param(id, "size"));
        const float rise = kind == "torus" ? std::min(size.y, 0.5f * size.x) * 0.5f : 0.5f * size.y;
        net_.setParam(id, "center", pv(at + Vec3(0.0f, rise, 0.0f)));
        net_.setParam(id, "color", pv(kPalette[newColor_++ % 6]));
        net_.rename(id, net_.uniqueName(kind));
        linkIntoSolvers(id, "collider", "colliders");
    }
    if (kind == "smoke" || kind == "fire") {
        const int solver = ensurePyroChain();
        id = net_.add("pyro_source", slot.x, slot.y);
        const bool fire = kind == "fire";
        net_.setParam(id, "center", pv(at + Vec3(0.0f, 0.12f, 0.0f)));
        net_.setParam(id, fire ? "fuel" : "smoke", {fire ? 14.0f : 5.0f, 0.0f, 0.0f});
        net_.setParam(id, "heat", {fire ? 1.0f : 2.5f, 0.0f, 0.0f});
        net_.setParam(id, "velocity", {0.0f, fire ? 0.4f : 0.55f, 0.0f});
        net_.setParam(id, "flicker", {fire ? 0.7f : 0.2f, 0.0f, 0.0f});
        net_.rename(id, net_.uniqueName(kind));
        net_.connect(id, "source", solver, "sources");
        // A fire of its own wants turbulence where it is hot: give it some if
        // there is none yet.
        bool turbulent = false;
        for (const sim::Link& l : net_.linksInto(solver, "forces")) {
            const sim::Node* f = net_.node(l.from);
            turbulent = turbulent || (f && f->type == "turbulence");
        }
        if (!turbulent) {
            const int t = net_.add("turbulence", slot.x, slot.y + 96.0f);
            net_.connect(t, "force", solver, "forces");
        }
    }
    if (kind == "water_block" || kind == "fountain" || kind == "hose") {
        // Water: a block let go where the menu was opened, a jet straight
        // up from the floor, or one sideways from half a metre up.
        const int solver = ensureLiquidChain();
        id = net_.add("water_source", slot.x, slot.y);
        if (kind == "water_block") {
            net_.setParam(id, "size", pv(Vec3(0.4f, 0.5f, 0.4f)));
            net_.setParam(id, "center", pv(at + Vec3(0.0f, 0.25f, 0.0f)));
        } else {
            net_.setParam(id, "shape", {static_cast<float>(sim::Shape::Cylinder), 0.0f, 0.0f});
            net_.setParam(id, "mode", {1.0f, 0.0f, 0.0f});
            net_.setParam(id, "size", pv(Vec3(0.08f, 0.08f, 0.08f)));
            if (kind == "fountain") {
                net_.setParam(id, "center", pv(at + Vec3(0.0f, 0.05f, 0.0f)));
                net_.setParam(id, "velocity", pv(Vec3(0.0f, 3.0f, 0.0f)));
            } else {
                // Along its own y, turned to point along x.
                net_.setParam(id, "center", pv(at + Vec3(0.0f, 0.5f, 0.0f)));
                net_.setParam(id, "rotation", pv(Vec3(0.0f, 0.0f, -90.0f)));
                net_.setParam(id, "velocity", pv(Vec3(0.0f, 2.0f, 0.0f)));
            }
        }
        net_.rename(id, net_.uniqueName(kind == "water_block" ? "water" : kind));
        net_.connect(id, "water", solver, "sources");
    }
    if (kind == "rain" || kind == "rainstorm") id = addRain(kind == "rainstorm");
    for (const char* force : {"wind", "vortex", "turbulence", "attractor", "drag"}) {
        if (kind != force) continue;
        id = net_.add(force, slot.x, slot.y);
        if (kind == "vortex" || kind == "attractor") {
            net_.setParam(id, "center", pv(Vec3(at.x, 0.5f * sceneBox().size().y, at.z)));
        }
        linkIntoSolvers(id, "force", "forces");
    }
    if (!id) return 0;
    canvas_.select(id);
    canvas_.reveal(id);
    if (tool_ == GizmoMode::Select) tool_ = GizmoMode::Move;
    const sim::Node* n = net_.node(id);
    setMessage("Added " + (n ? n->name : what) + ": W, E, R move, turn and size it");
    return id;
}

bool SimWorkspace::sceneMenu(const Vec3& at) {
    struct Item {
        const char* kind;
        const char* label;
        Icon icon;
    };
    const ImU32 objectColor = IM_COL32(120, 150, 210, 255), sourceColor = IM_COL32(240, 142, 60, 255),
                forceColor = IM_COL32(70, 200, 215, 255);
    bool added = false;
    auto items = [&](const char* title, std::initializer_list<Item> list, ImU32 color) {
        ImGui::SeparatorText(title);
        for (const Item& it : list) {
            if (iconItem(it.icon, color, it.label)) {
                addToScene(it.kind, at);
                added = true;
            }
        }
    };
    items("Objects",
          {{"sphere", "Sphere", Icon::Sphere}, {"box", "Box", Icon::Box}, {"cylinder", "Cylinder", Icon::Cylinder},
           {"cone", "Cone", Icon::Cone}, {"torus", "Torus", Icon::Torus}},
          objectColor);
    if (iconItem(Icon::File, objectColor, "Mesh\xe2\x80\xa6")) {
        // An OBJ file, placed where the menu was opened.
        addAt_ = at;
        files_.open("Import a mesh", {".obj"}, false, folder());
        fileAction_ = FileAction::ImportMesh;
        added = true;
    }
    ImGui::SetItemTooltip("An OBJ file: a rock, a statue, a car -- in the scene, colliding");
    items("Sources", {{"fire", "Fire", Icon::Source}, {"smoke", "Smoke", Icon::Source}}, sourceColor);
    items("Water",
          {{"water_block", "Block of Water", Icon::Drop}, {"fountain", "Fountain", Icon::Drop},
           {"hose", "Hose", Icon::Drop}},
          IM_COL32(64, 170, 250, 255));
    items("Weather", {{"rain", "Rain", Icon::Rain}, {"rainstorm", "Rainstorm", Icon::Rain}},
          IM_COL32(150, 172, 210, 255));
    ImGui::SeparatorText("Shot");
    if (iconItem(Icon::Camera, IM_COL32(205, 208, 216, 255), "Camera")) {
        addCamera();
        setThroughCamera(true);
        added = true;
    }
    ImGui::SetItemTooltip("A camera that sees what the view sees, into the Output: renders go through it");
    items("Forces",
          {{"wind", "Wind", Icon::Wind}, {"vortex", "Vortex", Icon::Vortex}, {"turbulence", "Turbulence", Icon::Force},
           {"attractor", "Attractor", Icon::Attractor}, {"drag", "Drag", Icon::Force}},
          forceColor);
    return added;
}

int SimWorkspace::addMesh(const std::string& path, const Vec3& at) {
    std::string why;
    const auto mesh = sim::loadMesh(path, why);
    if (!mesh) {
        setMessage(why, true);
        return 0;
    }
    // As big as it is -- unless it is far too big or too small for a scene
    // metres across: then some 80 cm at its longest.
    Vec3 size = mesh->half() * 2.0f;
    const float longest = std::max({size.x, size.y, size.z});
    if (longest > 3.0f || longest < 0.05f) size = size * (0.8f / longest);
    const ImVec2 slot = freeSlot();
    const int id = net_.add("object", slot.x, slot.y);
    net_.setParam(id, "shape", {static_cast<float>(sim::Shape::Mesh), 0.0f, 0.0f});
    // A path under the network's folder is kept relative to it: the two
    // travel together.
    std::string stored = path;
    std::error_code ec;
    if (!folder().empty()) {
        const auto relative = std::filesystem::relative(path, folder(), ec);
        if (!ec && !relative.empty() && relative.string().rfind("..", 0) != 0) stored = relative.generic_string();
    }
    net_.setText(id, "file", stored);
    net_.setParam(id, "size", pv(size));
    net_.setParam(id, "center", pv(at + Vec3(0.0f, 0.5f * size.y, 0.0f)));
    net_.setParam(id, "color", pv(kPalette[newColor_++ % 6]));
    // Named after its file, as far as a name allows.
    std::string stem = std::filesystem::path(path).stem().string();
    for (char& c : stem) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    if (stem.empty() || std::isdigit(static_cast<unsigned char>(stem[0]))) stem = "mesh_" + stem;
    net_.rename(id, net_.uniqueName(stem));
    linkIntoSolvers(id, "collider", "colliders");
    canvas_.select(id);
    canvas_.reveal(id);
    char text[160];
    std::snprintf(text, sizeof text, "Imported %s: %zu triangles, %.2f \xc3\x97 %.2f \xc3\x97 %.2f m",
                  std::filesystem::path(path).filename().string().c_str(), mesh->mesh().triangles.size(),
                  static_cast<double>(size.x), static_cast<double>(size.y), static_cast<double>(size.z));
    setMessage(text);
    return id;
}

// --- the shot's camera --------------------------------------------------------------------

void SimWorkspace::setThroughCamera(bool on) {
    if (on && !compiled_.hasCamera) {
        setMessage("No camera: Shift+A > Camera adds one that sees what the view sees", true);
        on = false;
    }
    if (on == throughCamera_) return;
    throughCamera_ = on;
    if (!on) {
        // The viewport's own lens again, the horizon level.
        renderer_.orbit.roll = 0.0f;
        renderer_.orbit.fovY = gl::VolumeRenderer::kFovY;
    }
    guidesRevision_ = ~0ull;  // its frustum: hidden while looked through
    viewDirty_ = true;
}

int SimWorkspace::addCamera() {
    // Beside the Output, which it goes into.
    int output = 0;
    for (const sim::Node& n : net_.nodes()) {
        if (n.type == "output" && !output) output = n.id;
    }
    ImVec2 at(490.0f, 40.0f);
    if (const sim::Node* out = net_.node(output)) at = ImVec2(out->x, out->y + 150.0f);
    const int id = net_.add("camera", at.x, at.y);
    if (!output) output = net_.add("output", 720.0f, 40.0f);
    net_.connect(id, "camera", output, "camera");
    const sim::Camera c = gl::cameraFrom(renderer_.orbit, sim::Camera());
    net_.setParam(id, "center", pv(c.position));
    net_.setParam(id, "rotation", pv(c.rotation));
    canvas_.select(id);
    canvas_.reveal(id);
    recompile();
    const sim::Node* n = net_.node(id);
    setMessage("Added " + (n ? n->name : std::string("a camera")) +
               ": renders go through it; 0 looks through it, Ctrl+Alt+0 moves it to the view");
    return id;
}

void SimWorkspace::cameraFromView() {
    if (!compiled_.hasCamera || !net_.node(compiled_.camera.node)) {
        addCamera();
        return;
    }
    const int id = compiled_.camera.node;
    const sim::Camera c = gl::cameraFrom(renderer_.orbit, compiled_.cameraAt(current_));
    net_.setParamAt(id, "center", static_cast<float>(current_), pv(c.position));
    net_.setParamAt(id, "rotation", static_cast<float>(current_), pv(c.rotation));
    recompile();
    setMessage(net_.node(id)->name + " sees what the view sees");
}

// --- the camera frames the selection ----------------------------------------------------

void SimWorkspace::frameSelection() {
    setThroughCamera(false);
    Vec3 lo(1e30f), hi(-1e30f);
    bool any = false;
    auto grow = [&](const Vec3& a, const Vec3& b) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], a[k]);
            hi[k] = std::max(hi[k], b[k]);
        }
        any = true;
    };
    // Elements picked: their points.
    if (editingElements() && elementCount() > 0) {
        if (const GeometryPtr geo = renderer_.geometry()) {
            const std::vector<uint8_t> points = elementPoints(*geo);
            const auto P = geo->positions();
            for (size_t i = 0; i < points.size() && i < P.size(); ++i) {
                if (points[i]) grow(P[i], P[i]);
            }
        }
    }
    const std::set<int> chosen = editingElements() ? std::set<int>() : canvas_.selection();
    for (const sim::Solid& s : compiled_.solidsAt(current_)) {
        if (!chosen.count(s.body.node)) continue;
        Vec3 a, b;
        s.body.instance().bounds(a, b);
        grow(a, b);
    }
    if (compiled_.ok && compiled_.world.hasGas) {
        for (const sim::Emitter& e : compiled_.world.gas.emitters) {
            if (!chosen.count(e.node)) continue;
            Vec3 a, b;
            e.shapeAt(0.0f).bounds(a, b);
            grow(a, b);
        }
    }
    if (compiled_.ok && compiled_.world.hasWater) {
        for (const sim::WaterSource& w : compiled_.world.water.sources) {
            if (!chosen.count(w.node)) continue;
            Vec3 a, b;
            w.instance().bounds(a, b);
            grow(a, b);
        }
    }
    for (const int id : chosen) {
        const sim::Node* n = net_.node(id);
        if (n && n->type == "rain") {
            // The cloud, and the ground it rains on.
            const Vec3 c = v3(net_.param(id, "center")), half = v3(net_.param(id, "size")) * 0.5f;
            grow(Vec3(c.x - half.x, 0.0f, c.z - half.z), c + half);
            continue;
        }
        if (n && n->display) {
            // The geometry shown.
            Vec3 a, b;
            if (renderer_.geometryBounds(a, b)) grow(a, b);
            continue;
        }
        if (!n || n->type == "object" || n->type == "pyro_source" || n->type == "water_source") continue;
        Vec3 c;
        sim::Rotation f;
        if (!placeOf(id, c, f)) continue;
        const sim::Handles& h = sim::findNodeType(n->type)->handles;
        const float r = h.radius ? net_.value(id, h.radius) : 0.3f;
        grow(c - Vec3(r), c + Vec3(r));
    }
    if (!any) {
        // Nothing selected: the domains, every object and the geometry
        // shown -- only that, when nothing is simulated.
        if (compiled_.ok) {
            const sim::Domain dm = sceneBox();
            grow(dm.origin(), dm.origin() + dm.size());
        }
        for (const sim::Solid& s : compiled_.solidsAt(current_)) {
            Vec3 a, b;
            s.body.instance().bounds(a, b);
            grow(a, b);
        }
        Vec3 a, b;
        if (renderer_.geometryBounds(a, b)) grow(a, b);
        if (!any) {
            const sim::Domain dm = sceneBox();
            grow(dm.origin(), dm.origin() + dm.size());
        }
    }
    const Vec3 middle = (lo + hi) * 0.5f;
    const float radius = std::max(0.5f * length(hi - lo), 0.05f);
    gl::Orbit& o = renderer_.orbit;
    for (int k = 0; k < 3; ++k) o.target[k] = middle[k];
    o.distance = std::clamp(1.15f * radius / std::sin(o.fovY * 3.14159265f / 360.0f), 0.2f, 200.0f);
    framed_ = true;
    viewDirty_ = true;
}

// --- the toolbar and the menu ---------------------------------------------------------------

void SimWorkspace::viewTools(ImVec2 at, float bottom) {
    // 18 buttons in four groups, down to `bottom` at most: smaller buttons
    // where the view is short, and past that a second column.
    const int buttons = 18, groups = 4;
    const float gap = theme::px(2.0f), pad = theme::px(3.0f), space = theme::px(8.0f);
    const float fixed = static_cast<float>(buttons - groups) * gap + static_cast<float>(groups - 1) * space + 2.0f * pad;
    const float room = std::max(bottom - at.y, theme::px(120.0f));
    const float side = std::clamp((room - fixed) / static_cast<float>(buttons), theme::px(20.0f), theme::px(28.0f));
    const float top = at.y + pad, limit = at.y + room - pad;
    float x = at.x + pad, y = top, lowest = top;
    auto place = [&]() {
        if (y + side > limit + 0.5f && y > top) {  // on in the next column
            x += side + gap;
            y = top;
        }
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        y += side + gap;
        lowest = std::max(lowest, y);
    };
    auto group = [&]() {
        if (y > top) y += space - gap;
    };
    // The buttons over their panel, which is drawn behind them once its
    // size is known.
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImDrawListSplitter layers;
    layers.Split(d, 2);
    layers.SetCurrentChannel(d, 1);
    struct Tool {
        GizmoMode mode;
        Icon icon;
        const char* tip;
    };
    const Tool tools[] = {{GizmoMode::Select, Icon::Select, "Select (Q): click an object, a source, a guide; Shift adds"},
                          {GizmoMode::Move, Icon::Move, "Move (W): drag an arrow, a square, the dot"},
                          {GizmoMode::Rotate, Icon::Rotate, "Rotate (E): drag a ring"},
                          {GizmoMode::Scale, Icon::Scale, "Scale (R): drag a cube; the middle one sizes all three"}};
    for (const Tool& t : tools) {
        place();
        if (theme::iconButton(t.tip, t.icon, t.tip, tool_ == t.mode && !paint_, true, side)) {
            tool_ = t.mode;
            setPaint(false);
        }
    }
    // Soft selection: what the handle's drags take along.
    place();
    const Soft soft = softNow();
    if (theme::iconButton("soft", Icon::Soft,
                          "Soft selection (O): a drag of the handle takes the points round what is picked along, less "
                          "the further they are; [ ] or the wheel while dragging: the radius",
                          soft.on && editingElements() && !paint_, editingElements() && !paint_, side)) {
        setSoft(!soft.on);
    }
    group();
    // What a click picks: objects, or the displayed geometry's points,
    // edges, primitives; and the brush.
    struct Kind {
        Elements mode;
        Icon icon;
        const char* tip;
    };
    const Kind kinds[] = {
        {Elements::Objects, Icon::Geometry, "Objects (1): a click picks an object, a source, a guide"},
        {Elements::Points, Icon::Vertices, "Points (2): pick the displayed geometry's points -- a click, a box; Shift "
                                           "adds, Ctrl takes away; Alt+drag turns the view"},
        {Elements::Edges, Icon::Edges, "Edges (3): pick its edges"},
        {Elements::Primitives, Icon::Faces, "Primitives (4): pick its polygons and curves"}};
    for (const Kind& k : kinds) {
        place();
        if (theme::iconButton(k.tip, k.icon, k.tip, elements_ == k.mode && !paint_, true, side)) {
            setPaint(false);
            setElements(k.mode);
        }
    }
    // How a drag picks them -- a click goes on to the next way; and whether
    // what is hidden is picked too.
    place();
    const bool picking = editingElements() && !paint_;
    const Icon styleIcon = pickStyle_ == PickStyle::Lasso   ? Icon::Lasso
                           : pickStyle_ == PickStyle::Brush ? Icon::PickBrush
                                                            : Icon::PickBox;
    const char* styleTip = pickStyle_ == PickStyle::Lasso
                               ? "Lasso (S): a drag draws round what it picks. Click: the brush"
                           : pickStyle_ == PickStyle::Brush
                               ? "Brush (S): what it goes over is picked; [ ] or Shift+wheel sizes it. Click: the box"
                               : "Box (S): a drag picks what the box holds. Click: the lasso";
    if (theme::iconButton("pickstyle", styleIcon, styleTip, false, picking, side)) {
        setPickStyle(pickStyle_ == PickStyle::Box ? PickStyle::Lasso : pickStyle_ == PickStyle::Lasso ? PickStyle::Brush
                                                                                                  : PickStyle::Box);
    }
    place();
    if (theme::iconButton("hidden", Icon::XRay,
                          "Hidden too (H): what the surface hides -- the far side, points behind -- is picked as well, "
                          "and drawn faint",
                          pickHidden_, picking, side)) {
        setPickHidden(!pickHidden_);
    }
    place();
    if (theme::iconButton("paint", Icon::Brush,
                          "Paint (P): an attribute on the displayed geometry -- pin, tear, mass -- with a brush; Ctrl "
                          "paints the Erase Value, [ ] sizes it",
                          paint_ && !sculpting(), true, side)) {
        setPaint(!(paint_ && !sculpting()));
    }
    place();
    if (theme::iconButton("sculpt", Icon::Sculpt,
                          "Sculpt (U): push and pull the displayed geometry with a brush, smooth it (Shift), grab it, "
                          "flatten it -- a Sculpt node; Ctrl pulls in, [ ] sizes it",
                          sculpting(), true, side)) {
        setSculpt(!sculpting());
    }
    place();
    if (theme::iconButton("numbers", Icon::Numbers, "Numbers (N): of the points, or of the primitives, seen", numbers_,
                          editingElements(), side)) {
        numbers_ = !numbers_;
        numbersKey_.clear();
    }
    group();
    place();
    if (theme::iconButton("axes", localAxes_ ? Icon::Local : Icon::World,
                          localAxes_ ? "Local axes: the gizmo turns with the object. Click for the world's."
                                     : "World axes. Click for the object's own.",
                          false, true, side)) {
        localAxes_ = !localAxes_;
    }
    place();
    if (theme::iconButton("snap", Icon::Magnet, "Snap: 5 cm, 15\xc2\xb0, \xc3\x97" "0.1 at a time (Ctrl while dragging turns it the other way)",
                          snap_, true, side)) {
        snap_ = !snap_;
    }
    group();
    place();
    if (theme::iconButton("add", Icon::Plus, "Add an object, a source, a force (Shift+A)", false, true, side)) {
        addAt_ = floorPoint(camera_, ImVec2(camera_.lo.x + camera_.size.x * 0.5f, camera_.lo.y + camera_.size.y * 0.5f));
        ImGui::OpenPopup("add_to_scene");
    }
    place();
    if (theme::iconButton("frame", Icon::Frame, "Frame the selection (F), or everything", false, true, side)) {
        frameSelection();
    }
    layers.SetCurrentChannel(d, 0);
    toolsLo_ = at;
    toolsHi_ = ImVec2(x + side + pad, lowest - gap + pad);
    d->AddRectFilled(toolsLo_, toolsHi_, IM_COL32(24, 25, 29, 215), theme::px(7.0f));
    layers.Merge(d);
}

void SimWorkspace::viewMenu() {
    if (editingElements()) {
        // What is picked of the geometry: a group of it, deleted, all of it.
        const bool any = elementCount() > 0;
        if (ImGui::MenuItem("Group from Picked", "Ctrl+G", false, any)) groupElements();
        if (iconItem(Icon::Trash, theme::kTextDim, "Delete Picked", "Del") && any) deleteElements();
        if (ImGui::MenuItem("Dissolve Picked", "Ctrl+X", false,
                            any && (elements_ == Elements::Edges || elements_ == Elements::Primitives))) {
            dissolveElements();
        }
        if (ImGui::MenuItem("Extrude Picked", nullptr, false, any && elements_ == Elements::Primitives)) {
            applyToPicked("polyextrude");
        }
        if (ImGui::BeginMenu("Node on Picked", net_.node(net_.displayed()) != nullptr)) {
            if (pickedMenu()) ImGui::CloseCurrentPopup();
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Pick All", "Ctrl+A", false, !paint_)) selectAllElements(false);
        if (ImGui::MenuItem("Pick the Others", "Ctrl+I", false, !paint_)) selectAllElements(true);
        ImGui::Separator();
        // Soft selection: on, how far is measured, the falloff's shape.
        const Soft soft = softNow();
        if (ImGui::MenuItem("Soft Selection", "O", soft.on, !paint_)) setSoft(!soft.on);
        if (ImGui::BeginMenu("Soft Distance", !paint_)) {
            if (ImGui::MenuItem("Space", nullptr, soft.metric == 0)) setSoftMetric(0);
            if (ImGui::MenuItem("Along the Surface", nullptr, soft.metric == 1)) setSoftMetric(1);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Soft Falloff", !paint_)) {
            const char* shapes[5] = {"Smooth", "Linear", "Sharp", "Sphere", "Constant"};
            for (int k = 0; k < 5; ++k) {
                if (ImGui::MenuItem(shapes[k], nullptr, soft.falloff == k)) setSoftFalloff(k);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
    }
    if (ImGui::BeginMenu("Add")) {
        if (sceneMenu(addAt_)) ImGui::CloseCurrentPopup();
        ImGui::EndMenu();
    }
    const std::vector<int> chosen(canvas_.selection().begin(), canvas_.selection().end());
    ImGui::Separator();
    if (iconItem(Icon::Copy, theme::kTextDim, "Duplicate", "Ctrl+D") && !chosen.empty()) duplicate(chosen);
    if (iconItem(Icon::Trash, theme::kTextDim, "Delete", "Del") && !chosen.empty()) {
        removeNodes(chosen);
        canvas_.clearSelection();
    }
    if (iconItem(Icon::Bypass, theme::kTextDim, "Bypass", "B") && !chosen.empty()) toggleBypass(chosen);
    ImGui::Separator();
    if (ImGui::MenuItem("Look Through the Camera", "0", throughCamera_, compiled_.hasCamera)) {
        setThroughCamera(!throughCamera_);
    }
    if (ImGui::MenuItem("Camera from View", "Ctrl+Alt+0")) cameraFromView();
    if (iconItem(Icon::Frame, theme::kTextDim, "Frame", "F")) frameSelection();
    ImGui::Separator();
    if (iconItem(Icon::Select, tool_ == GizmoMode::Select ? theme::kAccent : theme::kTextDim, "Select", "Q")) tool_ = GizmoMode::Select;
    if (iconItem(Icon::Move, tool_ == GizmoMode::Move ? theme::kAccent : theme::kTextDim, "Move", "W")) tool_ = GizmoMode::Move;
    if (iconItem(Icon::Rotate, tool_ == GizmoMode::Rotate ? theme::kAccent : theme::kTextDim, "Rotate", "E")) tool_ = GizmoMode::Rotate;
    if (iconItem(Icon::Scale, tool_ == GizmoMode::Scale ? theme::kAccent : theme::kTextDim, "Scale", "R")) tool_ = GizmoMode::Scale;
    ImGui::Separator();
    if (iconItem(Icon::Geometry, elements_ == Elements::Objects && !paint_ ? theme::kAccent : theme::kTextDim, "Objects", "1")) {
        setPaint(false);
        setElements(Elements::Objects);
    }
    if (iconItem(Icon::Vertices, elements_ == Elements::Points && !paint_ ? theme::kAccent : theme::kTextDim, "Points", "2")) {
        setPaint(false);
        setElements(Elements::Points);
    }
    if (iconItem(Icon::Edges, elements_ == Elements::Edges && !paint_ ? theme::kAccent : theme::kTextDim, "Edges", "3")) {
        setPaint(false);
        setElements(Elements::Edges);
    }
    if (iconItem(Icon::Faces, elements_ == Elements::Primitives && !paint_ ? theme::kAccent : theme::kTextDim, "Primitives", "4")) {
        setPaint(false);
        setElements(Elements::Primitives);
    }
    if (iconItem(Icon::Brush, paint_ && !sculpting() ? theme::kAccent : theme::kTextDim, "Paint", "P")) {
        setPaint(!(paint_ && !sculpting()));
    }
    if (iconItem(Icon::Sculpt, sculpting() ? theme::kAccent : theme::kTextDim, "Sculpt", "U")) setSculpt(!sculpting());
    if (sculpting() && ImGui::BeginMenu("Sculpt Tool")) {
        const char* tools[4] = {"Push / Pull", "Smooth", "Grab", "Flatten"};
        const int node = sculptNode();
        const int now = static_cast<int>(net_.valueAt(node, "tool", static_cast<float>(current_))[0]);
        for (int k = 0; k < 4; ++k) {
            if (ImGui::MenuItem(tools[k], nullptr, now == k)) net_.setParam(node, "tool", {static_cast<float>(k), 0.0f, 0.0f});
        }
        ImGui::EndMenu();
    }
    if (editingElements() && !paint_ && ImGui::BeginMenu("Pick With")) {
        if (iconItem(Icon::PickBox, pickStyle_ == PickStyle::Box ? theme::kAccent : theme::kTextDim, "A Box", "S")) {
            setPickStyle(PickStyle::Box);
        }
        if (iconItem(Icon::Lasso, pickStyle_ == PickStyle::Lasso ? theme::kAccent : theme::kTextDim, "A Lasso", "S")) {
            setPickStyle(PickStyle::Lasso);
        }
        if (iconItem(Icon::PickBrush, pickStyle_ == PickStyle::Brush ? theme::kAccent : theme::kTextDim, "A Brush", "S")) {
            setPickStyle(PickStyle::Brush);
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Pick Hidden Too", "H", pickHidden_, editingElements() && !paint_)) setPickHidden(!pickHidden_);
    if (ImGui::MenuItem("Numbers", "N", numbers_, editingElements())) {
        numbers_ = !numbers_;
        numbersKey_.clear();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Local Axes", nullptr, localAxes_)) localAxes_ = !localAxes_;
    if (ImGui::MenuItem("Snap", nullptr, snap_)) snap_ = !snap_;
    if (ImGui::MenuItem("Guides", "G", guides_)) {
        guides_ = !guides_;
        guidesRevision_ = ~0ull;
    }
}

void SimWorkspace::viewKeys(bool overView) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (gizmo_.dragging()) {
            gizmo_.cancel();
            if (editNode_ || editPending_) {
                restoreElementDrag();
            } else if (extrudeNode_) {
                net_.setParamAt(extrudeNode_, "distance", static_cast<float>(current_), {extrudeDistance0_, 0.0f, 0.0f});
                extrudeNode_ = 0;
            } else {
                restoreDrag();
            }
            gizmoOwnsMouse_ = true;  // until the button is let go
            setMessage("Put back");
        } else if (grabbing_) {
            // Sculpting's grab: the strokes as they were before it.
            if (const int node = sculptNode()) net_.setText(node, "strokes", grabBefore_);
            grabbing_ = stroking_ = false;
            pressCancelled_ = true;
        } else if (brushing_) {
            // The brush's stroke: what was picked before it.
            const uint64_t revision = picked_.revision;
            picked_ = brushBefore_;
            picked_.revision = revision + 1;
            brushing_ = false;
            pressCancelled_ = true;
        } else if (boxing_) {
            boxing_ = false;
            lasso_.clear();
            pressCancelled_ = true;
        } else if (overView && paint_) {
            setPaint(false);
        } else if (overView && editingElements() && elementCount() > 0) {
            picked_.mask.clear();
            picked_.edges.clear();
            ++picked_.revision;
        } else if (overView) {
            canvas_.clearSelection();
        }
    }
    if (!overView || gizmo_.dragging() || stroking_ || brushing_) return;
    const std::vector<int> chosen(canvas_.selection().begin(), canvas_.selection().end());
    const bool zero = ImGui::IsKeyPressed(ImGuiKey_0, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad0, false);
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_D, false) && !chosen.empty()) duplicate(chosen);
        if (io.KeyAlt && zero) cameraFromView();
        if (editingElements() && !io.KeyAlt) {
            if (ImGui::IsKeyPressed(ImGuiKey_G, false)) groupElements();
            if (ImGui::IsKeyPressed(ImGuiKey_X, false) && !paint_) dissolveElements();
            if (ImGui::IsKeyPressed(ImGuiKey_A, false) && !paint_) selectAllElements(false);
            if (ImGui::IsKeyPressed(ImGuiKey_I, false) && !paint_) selectAllElements(true);
        }
        return;
    }
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        addAt_ = floorPoint(camera_, io.MousePos);
        ImGui::OpenPopup("add_to_scene");
        return;
    }
    if (io.KeyAlt || io.KeyShift) return;
    // What a click picks: objects, points, edges, primitives; the brush.
    const Elements modes[4] = {Elements::Objects, Elements::Points, Elements::Edges, Elements::Primitives};
    for (int k = 0; k < 4; ++k) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + k), false)) {
            setPaint(false);
            setElements(modes[k]);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) setPaint(!(paint_ && !sculpting()));
    if (ImGui::IsKeyPressed(ImGuiKey_U, false)) setSculpt(!sculpting());
    // How a drag picks: a box, a lasso, a brush; what is hidden too.
    if (ImGui::IsKeyPressed(ImGuiKey_S, false) && editingElements() && !paint_) {
        setPickStyle(pickStyle_ == PickStyle::Box ? PickStyle::Lasso : pickStyle_ == PickStyle::Lasso ? PickStyle::Brush
                                                                                                  : PickStyle::Box);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_H, false) && editingElements() && !paint_) setPickHidden(!pickHidden_);
    if (ImGui::IsKeyPressed(ImGuiKey_O, false) && editingElements() && !paint_) setSoft(!softNow().on);
    if (ImGui::IsKeyPressed(ImGuiKey_N, false) && editingElements()) {
        numbers_ = !numbers_;
        numbersKey_.clear();
    }
    // Tab: a geometry node after the shown one, on what is picked.
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && net_.node(net_.displayed())) {
        setPaint(false);
        ImGui::OpenPopup("on_picked");
    }
    // [ ]: the size of what is in the hand -- the paint brush, the soft
    // radius round what is picked, the brush that picks.
    const bool smaller = ImGui::IsKeyPressed(ImGuiKey_LeftBracket), larger = ImGui::IsKeyPressed(ImGuiKey_RightBracket);
    if (smaller || larger) {
        const float k = smaller ? 0.8f : 1.25f;
        const Soft soft = softNow();
        if (paint_) {
            scaleBrush(k);
        } else if (editingElements() && soft.on && elementCount() > 0) {
            setSoftRadius(soft.radius * k);
        } else if (editingElements() && pickStyle_ == PickStyle::Brush) {
            pickBrush_ = std::clamp((pickBrush_ > 0.0f ? pickBrush_ : 24.0f) * k, 3.0f, 400.0f);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_K, false)) keySelection();
    const GizmoMode keyed[4] = {GizmoMode::Select, GizmoMode::Move, GizmoMode::Rotate, GizmoMode::Scale};
    const ImGuiKey keys[4] = {ImGuiKey_Q, ImGuiKey_W, ImGuiKey_E, ImGuiKey_R};
    for (int k = 0; k < 4; ++k) {
        if (!ImGui::IsKeyPressed(keys[k], false)) continue;
        tool_ = keyed[k];
        setPaint(false);  // a tool put in the hand puts the brush down
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frameSelection();
    if (zero) setThroughCamera(!throughCamera_);
    if (ImGui::IsKeyPressed(ImGuiKey_B, false) && !chosen.empty()) toggleBypass(chosen);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_X, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        if (editingElements()) {
            // Of the geometry, what is picked: a Blast.
            if (!paint_) deleteElements();
        } else if (!chosen.empty()) {
            removeNodes(chosen);
            canvas_.clearSelection();
        }
    }
}

// --- the viewport ------------------------------------------------------------------------

void SimWorkspace::viewport(ImVec2 size) {
    (void)size;
    const sim::Domain dm = sceneBox();
    const std::string info = gridsText();
    // Two tabs in the header: the viewport, and the render of what it shows.
    int tab = renderTabOn_ ? 1 : 0;
    ui::PanelHeader h = ui::tabHeader(Icon::Viewport, tab, {"Viewport", "Render"}, info.c_str());
    renderTabOn_ = tab == 1;
    if (ui::headerButton(h, "image", Icon::Camera, "Render this frame to a PNG\xe2\x80\xa6")) {
        files_.open("Render image", {".png"}, true, (fs::path(renderFolder()) / (stem() + ".png")).string());
        fileAction_ = FileAction::Image;
    }
    if (ui::headerButton(h, "video", Icon::Film, "Render the shot to a video\xe2\x80\xa6", false, compiled_.ok)) chooseVideo();
    if (ui::headerButton(h, "home", Icon::Viewport, "Frame the domain")) {
        setThroughCamera(false);
        framed_ = false;
    }
    if (ui::headerButton(h, "through", Icon::Eye,
                         compiled_.hasCamera ? "Look through the camera (0)" : "Look through the camera (0): add one first, "
                                                                                "Shift+A > Camera",
                         throughCamera_, compiled_.hasCamera)) {
        setThroughCamera(!throughCamera_);
    }
    if (ui::headerButton(h, "guides", Icon::Guides, "Guides: the domain, sources, forces (G)", guides_)) {
        guides_ = !guides_;
        guidesRevision_ = ~0ull;
        updateGuides();
    }

    // The camera frames the domain when a network opens and when the
    // domain's size changes -- not for another resolution; nothing
    // simulated, the geometry shown, once it has cooked. Both tabs see
    // through it.
    const Vec3 box = compiled_.ok && compiled_.world.any() ? dm.size() : framedSize_;
    const bool resized = std::fabs(box.x - framedSize_.x) + std::fabs(box.y - framedSize_.y) +
                             std::fabs(box.z - framedSize_.z) > 1e-4f;
    const bool simulated = compiled_.ok && compiled_.world.any();
    Vec3 glo, ghi;
    const bool geometry = !simulated && renderer_.geometryBounds(glo, ghi);
    if (!framed_ || resized || (geometry && !geometryFramed_)) {
        renderer_.orbit = gl::VolumeRenderer::viewOf(dm);
        if (geometry) {
            const Vec3 middle = (glo + ghi) * 0.5f;
            gl::Orbit& o = renderer_.orbit;
            for (int k = 0; k < 3; ++k) o.target[k] = middle[k];
            o.distance = std::clamp(1.15f * std::max(0.5f * length(ghi - glo), 0.05f) / std::sin(o.fovY * 3.14159265f / 360.0f),
                                    0.2f, 200.0f);
            geometryFramed_ = true;
        }
        framedSize_ = box;
        framed_ = true;
        viewDirty_ = true;
    }

    if (renderTabOn_) {
        const ImVec2 room = ImGui::GetContentRegionAvail();
        renderTab(static_cast<int>(room.x), static_cast<int>(room.y));
        return;
    }
    stopRender();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int w = std::max(16, static_cast<int>(avail.x)), hh = std::max(16, static_cast<int>(avail.y));
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    const ImVec2 hi(lo.x + static_cast<float>(w), lo.y + static_cast<float>(hh));
    const ImGuiIO& io = ImGui::GetIO();
    // Through the camera: its view, its picture (the gate) as big as the
    // viewport allows, the rest of the view round it.
    if (throughCamera_ && !compiled_.hasCamera) setThroughCamera(false);
    if (throughCamera_) {
        const sim::Camera& c = compiled_.cameraAt(current_);
        const float fw = static_cast<float>(w), fh = static_cast<float>(hh);
        float gw = fw, gh = fh;
        if (fw / fh > c.aspect()) gw = fh * c.aspect();
        else gh = fw / c.aspect();
        gl::Orbit through = gl::orbitThrough(c, focusOf(c));
        through.fovY = 2.0f * std::atan(std::tan(c.fovY() * 3.14159265f / 360.0f) * fh / gh) * 180.0f / 3.14159265f;
        const gl::Orbit& now = renderer_.orbit;
        if (through.yaw != now.yaw || through.pitch != now.pitch || through.distance != now.distance ||
            through.roll != now.roll || through.fovY != now.fovY || through.target[0] != now.target[0] ||
            through.target[1] != now.target[1] || through.target[2] != now.target[2]) {
            renderer_.orbit = through;
            viewDirty_ = true;
        }
        gateLo_ = ImVec2(lo.x + 0.5f * (fw - gw), lo.y + 0.5f * (fh - gh));
        gateHi_ = ImVec2(gateLo_.x + gw, gateLo_.y + gh);
        // The plate behind it: this frame's, filling the gate.
        const std::string plate = c.plateFile(current_);
        if (plate != shownPlate_ || !(c == plateCamera_)) {
            std::string why;
            if (!renderer_.setPlate(plate, c, why)) setMessage(why, true);
            shownPlate_ = plate;
            plateCamera_ = c;
            viewDirty_ = true;
        }
    } else if (renderer_.hasPlate()) {
        renderer_.clearPlate();
        shownPlate_.clear();
        viewDirty_ = true;
    }
    ViewCamera cam = ViewCamera::of(renderer_.orbit, lo, ImVec2(static_cast<float>(w), static_cast<float>(hh)));
    camera_ = cam;
    auto within = [](ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.y >= a.y && p.x < b.x && p.y < b.y; };
    const bool onTools = within(io.MousePos, toolsLo_, toolsHi_) || within(io.MousePos, noticeLo_, noticeHi_);
    const bool overView = ImGui::IsWindowHovered() && within(io.MousePos, lo, hi) && !onTools;

    // What is under the mouse lights up -- while nothing is being dragged:
    // an object, or the element of the displayed geometry.
    checkElements();
    const bool buttons = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                         ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    if (!buttons) {
        hovered_ = overView && !editingElements() && gizmo_.hovered() == Handle::None ? pickAt(cam, io.MousePos) : 0;
    }
    if (!editingElements() || paint_ || pickStyle_ == PickStyle::Brush || buttons || !overView ||
        gizmo_.hovered() != Handle::None || gizmo_.dragging()) {
        hoverElement_ = -1;
        hoverGeometry_ = nullptr;
    } else if (io.MousePos.x != hoverMouse_.x || io.MousePos.y != hoverMouse_.y || cam.eye != hoverEye_ ||
               cam.forward != hoverForward_ || renderer_.geometry().get() != hoverGeometry_) {
        hoverMouse_ = io.MousePos;
        hoverEye_ = cam.eye;
        hoverForward_ = cam.forward;
        hoverGeometry_ = renderer_.geometry().get();
        hoverElement_ = elementAt(cam, io.MousePos);
    }
    const std::vector<int> chosen(canvas_.selection().begin(), canvas_.selection().end());
    if (chosen != highlighted_ || hovered_ != highlightedHover_) {
        highlighted_ = chosen;
        highlightedHover_ = hovered_;
        renderer_.setHighlight(chosen, hovered_);
        viewDirty_ = true;
    }
    updateOverlay();

    if (w != viewWidth_ || hh != viewHeight_) viewDirty_ = true;
    if (viewDirty_ && rendererLog_.empty()) {
        renderer_.render(w, hh);
        viewWidth_ = w;
        viewHeight_ = hh;
        viewDirty_ = false;
    }
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(renderer_.colorTexture())),
                 ImVec2(static_cast<float>(w), static_cast<float>(hh)), ImVec2(0, 1), ImVec2(1, 0));
    ImGui::SetCursorScreenPos(lo);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("view", ImVec2(static_cast<float>(w), static_cast<float>(hh)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool viewActive = ImGui::IsItemActive(), viewHovered = ImGui::IsItemHovered(),
               viewReleased = ImGui::IsItemDeactivated();
    const bool pressed = ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (ImGui::IsItemActivated()) {
        // Picking elements or painting, the left button is theirs; held
        // with Alt or Space it turns the view.
        pressTurns_ = io.KeyAlt || ImGui::IsKeyDown(ImGuiKey_Space);
        pressCancelled_ = false;
    }
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->PushClipRect(lo, hi, true);
    if (throughCamera_) {
        // Outside the camera's picture, dimmed; its frame, and what it is.
        const ImU32 dim = IM_COL32(6, 6, 8, 160);
        d->AddRectFilled(lo, ImVec2(hi.x, gateLo_.y), dim);
        d->AddRectFilled(ImVec2(lo.x, gateHi_.y), hi, dim);
        d->AddRectFilled(ImVec2(lo.x, gateLo_.y), ImVec2(gateLo_.x, gateHi_.y), dim);
        d->AddRectFilled(ImVec2(gateHi_.x, gateLo_.y), ImVec2(hi.x, gateHi_.y), dim);
        d->AddRect(gateLo_, gateHi_, IM_COL32(225, 226, 232, 170), 0.0f, 0, theme::px(1.0f));
        const sim::Camera& c = compiled_.cameraAt(current_);
        const sim::Node* n = net_.node(c.node);
        char label[128];
        std::snprintf(label, sizeof label, "%s  \xc2\xb7  %.0f mm  \xc2\xb7  %d \xc3\x97 %d", n ? n->name.c_str() : "camera",
                      static_cast<double>(c.focal), c.width, c.height);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const float pad = theme::px(8.0f);
        d->AddText(ImVec2(gateHi_.x - ts.x - pad, gateLo_.y + pad), IM_COL32(225, 226, 232, 210), label);
    }

    // The gizmo, on what is selected: the objects -- or the elements picked,
    // through an Edit node; the brush.
    const std::vector<int> moving = editingElements() ? std::vector<int>() : movable();
    const GizmoMode tool = moving.empty() ? GizmoMode::Select : toolFor(moving);
    if (editingElements()) {
        elementGizmo(d, cam, overView);
        paintTool(d, cam, overView);
        pickBrushTool(d, cam, overView, pressed);
        // No handle: the soft radius round the middle of what is picked.
        Vec3 middle;
        if (tool_ == GizmoMode::Select && !paint_ && elementCenter(middle)) drawSoftRing(d, cam, middle);
    } else if (tool != GizmoMode::Select) {
        Vec3 pivot;
        sim::Rotation frame;
        pivotOf(moving, tool, pivot, frame);
        if (gizmo_.dragging() && tool != gizmo_.drag().mode) {
            // The tool changed under the drag: keep what it did.
            gizmo_.cancel();
            dragStart_.clear();
        }
        const bool free = overView && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        gizmo_.update(d, cam, tool, pivot, frame, free, snap_ != io.KeyCtrl);
        if (gizmo_.began()) {
            dragStart_.clear();
            for (const int id : moving) dragStart_.push_back(placedOf(id));
            dragPivot_ = pivot;
            gizmoOwnsMouse_ = true;
        }
        if (gizmo_.dragging() || gizmo_.ended()) applyDrag(gizmo_.drag());
        if (gizmo_.ended()) {
            dragStart_.clear();
            recompile();
        }
    } else if (gizmo_.dragging()) {
        gizmo_.cancel();  // what was dragged went away
        dragStart_.clear();
    }
    // While the handle is dragged or the brush paints, the simulation waits.
    runner_->hold(gizmo_.dragging() || stroking_);

    // The camera -- unless the press was the gizmo's, the box's, the brush's.
    gl::Orbit& o = renderer_.orbit;
    const bool leftTurns = !editingElements() || pressTurns_;
    if (viewActive && !gizmoOwnsMouse_ && !boxing_ && !stroking_) {
        const ImVec2 dlt = io.MouseDelta;
        const bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left) && leftTurns;
        if ((dlt.x != 0.0f || dlt.y != 0.0f) &&
            (left || ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right))) {
            setThroughCamera(false);  // moving the view leaves the camera where it is
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (left && io.KeyShift)) {
                // Pan: move what the camera looks at, in the plane of the screen.
                const float k = o.distance * 0.0018f;
                const Vec3 move = cam.right * (-dlt.x * k) + cam.up * (dlt.y * k);
                for (int a = 0; a < 3; ++a) o.target[a] += move[a];
            } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                o.distance = std::clamp(o.distance * std::exp(dlt.y * 0.006f), 0.2f, 200.0f);
            } else if (left) {
                o.yaw -= dlt.x * 0.35f;
                o.pitch = std::clamp(o.pitch + dlt.y * 0.35f, -89.0f, 89.0f);
            }
            viewDirty_ = true;
        }
    }
    const float wheel = io.MouseWheel != 0.0f ? io.MouseWheel : io.KeyShift ? io.MouseWheelH : 0.0f;
    if (viewHovered && paint_ && io.KeyShift && wheel != 0.0f) {
        scaleBrush(std::pow(1.15f, wheel));  // Shift+wheel: the brush's size
    } else if (gizmo_.dragging() && editingElements() && io.MouseWheel != 0.0f && softNow().on) {
        setSoftRadius(softNow().radius * std::pow(1.15f, io.MouseWheel));  // dragging: the soft radius
    } else if (viewHovered && editingElements() && pickStyle_ == PickStyle::Brush && io.KeyShift && wheel != 0.0f) {
        pickBrush_ = std::clamp((pickBrush_ > 0.0f ? pickBrush_ : 24.0f) * std::pow(1.15f, wheel), 3.0f, 400.0f);
    } else if (viewHovered && io.MouseWheel != 0.0f) {
        setThroughCamera(false);
        o.distance = std::clamp(o.distance * std::pow(0.88f, io.MouseWheel), 0.2f, 200.0f);
        viewDirty_ = true;
    }

    // Picking elements, a left drag draws a box -- or a lasso: what it holds
    // is picked. (The brush picks as it goes.)
    const float still = theme::px(4.0f) * theme::px(4.0f);
    if (editingElements() && !paint_ && pickStyle_ != PickStyle::Brush && viewActive && !gizmoOwnsMouse_ &&
        !pressTurns_ && !pressCancelled_ && !boxing_ && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] >= still) {
        boxing_ = true;
        lasso_.assign(1, Vec2(io.MouseClickedPos[ImGuiMouseButton_Left].x, io.MouseClickedPos[ImGuiMouseButton_Left].y));
    }
    if (boxing_ && pickStyle_ == PickStyle::Lasso) {
        // A point more of the line each time the mouse has gone a few pixels on.
        const ImVec2 b = io.MousePos;
        const Vec2& last = lasso_.back();
        const float step = theme::px(3.0f);
        if ((b.x - last.x) * (b.x - last.x) + (b.y - last.y) * (b.y - last.y) >= step * step) lasso_.emplace_back(b.x, b.y);
        std::vector<ImVec2> line;
        line.reserve(lasso_.size());
        for (const Vec2& p : lasso_) line.emplace_back(p.x, p.y);
        d->AddPolyline(line.data(), static_cast<int>(line.size()), IM_COL32(0, 0, 0, 140), 0, theme::px(3.0f));
        d->AddPolyline(line.data(), static_cast<int>(line.size()), IM_COL32(255, 205, 80, 230), 0, theme::px(1.5f));
        // Where it closes, back to its start.
        d->AddLine(line.back(), line.front(), IM_COL32(255, 205, 80, 110), theme::px(1.0f));
        if (viewReleased) {
            lasso_.emplace_back(b.x, b.y);
            regionElements(cam, ScreenRegion::lasso(lasso_), io.KeyShift, io.KeyCtrl);
        }
    } else if (boxing_) {
        const ImVec2 a = io.MouseClickedPos[ImGuiMouseButton_Left], b = io.MousePos;
        const ImVec2 lo2(std::min(a.x, b.x), std::min(a.y, b.y)), hi2(std::max(a.x, b.x), std::max(a.y, b.y));
        d->AddRectFilled(lo2, hi2, IM_COL32(255, 200, 60, 28));
        d->AddRect(lo2, hi2, IM_COL32(255, 205, 80, 220), 0.0f, 0, theme::px(1.0f));
        if (viewReleased) regionElements(cam, ScreenRegion::box(a.x, a.y, b.x, b.y), io.KeyShift, io.KeyCtrl);
    }
    if (boxing_ && (viewReleased || !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        boxing_ = false;
        lasso_.clear();
    }

    // A click that did not drag selects; a double click frames what it hit.
    if (viewReleased && !gizmoOwnsMouse_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < still) {
        if (editingElements()) {
            // With the brush, the press was a dab.
            if (!paint_ && !pressTurns_ && !pressCancelled_ && pickStyle_ != PickStyle::Brush) {
                clickElements(cam, io.MousePos, io.KeyShift, io.KeyCtrl);
            }
        } else {
            const int picked = pickAt(cam, io.MousePos);
            if (io.KeyShift || io.KeyCtrl) {
                if (picked) canvas_.toggle(picked);
            } else if (picked) {
                canvas_.select(picked);
            } else {
                canvas_.clearSelection();
            }
        }
    }
    if (viewHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !gizmoOwnsMouse_ && !editingElements()) {
        if (pickAt(cam, io.MousePos)) {
            frameSelection();
        } else {
            setThroughCamera(false);
            framed_ = false;
        }
    }
    // A right click that did not drag: the menu.
    if (viewHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < still) {
        const int picked = editingElements() ? 0 : pickAt(cam, io.MousePos);
        if (picked && !canvas_.selection().count(picked)) canvas_.select(picked);
        addAt_ = floorPoint(cam, io.MousePos);
        ImGui::OpenPopup("view_menu");
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) gizmoOwnsMouse_ = gizmo_.dragging();
    viewKeys(overView || (viewHovered && !onTools));

    // Overlays: what is shown, the state of things -- each on a dark pill,
    // read over a white sky as over a black floor.
    const float pad = theme::px(10.0f);
    const float line = ImGui::GetFontSize() * 1.6f;  // a pill's line
    if (!rendererLog_.empty()) ui::overlayText(d, ImVec2(lo.x + pad, lo.y + pad), theme::kRed, rendererLog_.c_str());
    char text[160];
    const int cached = runner_->cached();
    const float textY = lo.y + pad;
    if (shown_) {
        // Drawn coarser while it changes (View > Proxies): said so.
        std::snprintf(text, sizeof text, "Frame %d  \xc2\xb7  %.2f s%s", shown_->number, static_cast<double>(shown_->time),
                      shownProxy_ ? "  \xc2\xb7  proxy" : "");
        ui::overlayText(d, ImVec2(lo.x + pad, textY), IM_COL32(235, 236, 240, 240), text, theme::fonts().bold);
        if (current_ > cached) {
            std::snprintf(text, sizeof text, "simulating\xe2\x80\xa6 %d of %d", cached, current_);
            ui::overlayText(d, ImVec2(lo.x + pad, textY + line), theme::kAccentHover, text);
        } else if (shown_->number != current_ && levels_.empty()) {
            // Scrubbed to a frame on disk only: the last one shown stays
            // until it is read.
            std::snprintf(text, sizeof text, "reading frame %d\xe2\x80\xa6", current_);
            ui::overlayText(d, ImVec2(lo.x + pad, textY + line), theme::kAccentHover, text);
        }
    } else if (!levels_.empty() || editingAsset()) {
        // Inside an asset: its geometry, at the play head.
        std::snprintf(text, sizeof text, "Frame %d  \xc2\xb7  inside %s", current_,
                      (net_.asset().label.empty() ? net_.asset().name : net_.asset().label).c_str());
        ui::overlayText(d, ImVec2(lo.x + pad, textY), IM_COL32(235, 236, 240, 240), text, theme::fonts().bold);
    } else if (compiled_.ok) {
        ui::overlayText(d, ImVec2(lo.x + pad, textY), theme::kAccentHover,
                        gizmo_.dragging() ? "let go to simulate" : "simulating\xe2\x80\xa6");
    }
    // The toolbar goes under those two lines; the geometry still cooking, a
    // while after it was asked for, is said beside its top.
    const float toolsTop = textY + 2.0f * line;
    if (cooker_->busy() && ImGui::GetTime() - cookAsked_ > 0.25) {
        ui::overlayText(d, ImVec2(lo.x + pad + theme::px(48.0f), toolsTop + theme::px(4.0f)), theme::kAccentHover,
                        "cooking\xe2\x80\xa6");
    }
    // What is selected, or under the mouse, at the bottom.
    const int named = hovered_ ? hovered_ : canvas_.current();
    std::string elementsText = elementStatus();
    // Too wide for the viewport, the hints at its end go, one by one.
    const float room = static_cast<float>(w) - 2.0f * pad - theme::px(84.0f);
    for (size_t cut; ImGui::CalcTextSize(elementsText.c_str()).x > room &&
                     (cut = elementsText.rfind("  \xc2\xb7  ")) != std::string::npos;) {
        elementsText.erase(cut);
    }
    auto bottomRight = [&](const char* t, ImU32 color) {
        const ImVec2 ts = ImGui::CalcTextSize(t);
        ui::overlayText(d, ImVec2(hi.x - ts.x - pad - theme::px(4.0f), hi.y - ts.y - pad), color, t);
    };
    if (!elementsText.empty()) {
        bottomRight(elementsText.c_str(), hoverElement_ >= 0 ? theme::kText : theme::kAccentHover);
    } else if (const sim::Node* n = net_.node(named)) {
        const sim::NodeType* t = sim::findNodeType(n->type);
        std::snprintf(text, sizeof text, "%s  %s%s", n->name.c_str(), t ? t->label : n->type.c_str(),
                      hovered_ && hovered_ != canvas_.current() ? "" : "  \xc2\xb7  selected");
        bottomRight(text, hovered_ ? theme::kText : theme::kAccentHover);
    } else if (!emptyScene() && (!compiled_.solids.empty() || compiled_.ok)) {
        bottomRight("Click to select  \xc2\xb7  W E R move, rotate, scale  \xc2\xb7  Shift+A add", theme::kTextDim);
    }
    if (emptyScene()) {
        // Where to begin, in the middle of the view.
        const char* title = "An empty scene";
        const char* lines[] = {"Shift+A or a right click here: an object, fire, water, rain, a camera",
                               "Tab or a right click in the network: any node", "File > Examples: finished scenes"};
        float w = 0.0f;
        for (const char* l : lines) w = std::max(w, ImGui::CalcTextSize(l).x);
        const float line = ImGui::GetTextLineHeightWithSpacing();
        const ImVec2 c((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f - line * 2.0f);
        const float hh = line * 5.2f;
        d->AddRectFilled(ImVec2(c.x - w * 0.5f - pad * 2.0f, c.y - hh * 0.5f), ImVec2(c.x + w * 0.5f + pad * 2.0f, c.y + hh * 0.5f),
                         IM_COL32(20, 20, 24, 200), theme::px(8.0f));
        float y = c.y - hh * 0.5f + pad;
        ImGui::PushFont(theme::fonts().bold, 0.0f);
        d->AddText(ImVec2(c.x - ImGui::CalcTextSize(title).x * 0.5f, y), theme::kText, title);
        ImGui::PopFont();
        y += line * 1.4f;
        for (const char* l : lines) {
            d->AddText(ImVec2(c.x - ImGui::CalcTextSize(l).x * 0.5f, y), theme::kTextDim, l);
            y += line;
        }
    } else if (!compiled_.ok && !geometryOnly()) {
        std::string why = "Nothing to simulate";
        for (const sim::Problem& p : compiled_.problems) {
            if (p.level == sim::Problem::Level::Error) {
                why = p.message;
                break;
            }
        }
        const ImVec2 t = ImGui::CalcTextSize(why.c_str());
        const ImVec2 c((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        d->AddRectFilled(ImVec2(c.x - t.x * 0.5f - pad * 2.0f, c.y - t.y - pad), ImVec2(c.x + t.x * 0.5f + pad * 2.0f, c.y + t.y + pad),
                         IM_COL32(20, 20, 24, 210), theme::px(6.0f));
        theme::drawIcon(d, Icon::Warning, ImVec2(c.x, c.y - t.y * 0.45f), t.y * 1.1f, theme::kYellow);
        d->AddText(ImVec2(c.x - t.x * 0.5f, c.y + t.y * 0.25f), theme::kText, why.c_str());
    }
    drawNumbers(d, cam);
    drawGnomon(d, ImVec2(lo.x, hi.y));
    drawNotice(d, lo, hi);

    // The toolbar, on the left under the frame's number, ending above the
    // axes in the corner.
    viewTools(ImVec2(lo.x + pad, toolsTop), hi.y - theme::px(84.0f));
    d->PopClipRect();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::px(8.0f), theme::px(8.0f)));
    if (ImGui::BeginPopup("view_menu")) {
        viewMenu();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("add_to_scene")) {
        if (sceneMenu(addAt_)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("on_picked")) {
        if (pickedMenu()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

}  // namespace pg::editor
