// Editing the displayed geometry in the viewport, as in Houdini: its
// points, edges and primitives picked with the mouse; what is picked moved,
// turned and sized by the handle, made a group, deleted; an attribute
// painted on with a brush. Each is a node put after the displayed one --
// Edit, Group, Blast, Attribute Paint -- whose parameters hold what the
// mouse did: the elements as a pattern (pg/core/Selection.h), the strokes as
// dabs. What cannot be seen is not picked: the geometry's own surface hides
// what is behind it (pg/core/Pick.h) -- unless asked for (H).
//
//   1 2 3 4 5          objects; points, edges, primitives, vertices -- the corners
//                      of the primitives, each a dot a little inside its polygon
//   click, drag        pick one, or what a box holds: Shift adds, Ctrl takes away
//   S                  a drag draws a box, a lasso, or is a brush that picks what
//                      it goes over -- [ ] or Shift+wheel: its size
//   H                  what the surface hides is picked too, and drawn faint
//   Alt or Space held  the left button turns the view (middle: pan; right, wheel: zoom)
//   W E R              move, turn, size what is picked -- an Edit node; Q: no handle
//   O                  soft selection: the points round go along, less the further
//                      they are -- [ ], or the wheel while dragging: its radius
//   M                  symmetry: edits and brushes mirrored across x, y, z, off
//   Ctrl+G             a Group of it;   Delete, X: a Blast of it
//   Ctrl+X             edges or faces dissolved -- a Dissolve node
//   Ctrl+A, Ctrl+I     all of them, the others;   Escape: none
//   P                  the brush: the Attribute Paint node's attribute, its Value
//                      -- Ctrl: its Erase Value; [ ] or Shift+wheel: its size
//   U                  the sculpting brush -- a Sculpt node; Ctrl+D: dyntopo
//
// The marks -- the wire, the points, what is picked and what is under the
// mouse, the paint -- are the renderer's overlay: hidden behind what is in
// front of them, as the geometry is.
#include "SimWorkspace.h"

#include "pg/core/Mirror.h"
#include "pg/core/Soft.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace pg::editor {
namespace {

Vec3 v3(const sim::ParamValue& p) { return {p[0], p[1], p[2]}; }
sim::ParamValue pv(const Vec3& v) { return {v.x, v.y, v.z}; }

// The marks' colours.
const Vec4 kWire(0.03f, 0.03f, 0.04f, 0.5f);
const Vec4 kPoint(0.6f, 0.76f, 1.0f, 0.95f);
const Vec4 kPicked(1.0f, 0.8f, 0.2f, 1.0f);
const Vec4 kPickedFace(1.0f, 0.74f, 0.12f, 0.4f);
const Vec4 kHover(0.3f, 0.95f, 1.0f, 1.0f);
const Vec4 kHoverFace(0.3f, 0.95f, 1.0f, 0.25f);
const Vec4 kCorner(0.25f, 0.85f, 0.4f, 0.95f);

/// Past so many, the wire and the points are not drawn -- what is picked is.
constexpr size_t kMostMarks = 400000;
/// How long a picker begun is waited for before the frame goes on without
/// it: a small geometry's is made by then -- nothing goes and comes back.
constexpr std::chrono::milliseconds kPickerWait(8);
/// How much a picker refitted again and again may swell (ElementPicker::
/// swell) before a new one is made.
constexpr float kMostSwell = 4.0f;
/// How far to the right of the node before it a node put after one goes.
constexpr float kStep = 230.0f;

/// Pixels from the mouse an element is still under it.
float reach() { return theme::px(7.0f); }

const char* kindOf(SimWorkspace::Elements e, bool many) {
    switch (e) {
        case SimWorkspace::Elements::Points: return many ? "points" : "point";
        case SimWorkspace::Elements::Edges: return many ? "edges" : "edge";
        case SimWorkspace::Elements::Primitives: return many ? "primitives" : "primitive";
        case SimWorkspace::Elements::Vertices: return many ? "vertices" : "vertex";
        default: return many ? "objects" : "object";
    }
}

/// The colour a painted value shows as: blue for none, through cyan and
/// yellow, red for the most.
Vec4 paintColor(float v) {
    static const Vec4 stops[4] = {{0.1f, 0.22f, 0.95f, 0.35f}, {0.08f, 0.8f, 0.85f, 0.55f},
                                  {0.98f, 0.86f, 0.15f, 0.7f}, {1.0f, 0.18f, 0.08f, 0.85f}};
    const float t = std::clamp(v, 0.0f, 1.0f) * 3.0f;
    const int i = std::min(static_cast<int>(t), 2);
    const float f = t - static_cast<float>(i);
    const Vec4& a = stops[i];
    const Vec4& b = stops[i + 1];
    return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f};
}

/// The colour of a share of a drag, over the surface: none at 0, through
/// red to the colour of what is picked at 1.
Vec4 softColor(float w, float alpha) {
    const float t = std::clamp(w, 0.0f, 1.0f);
    return {0.95f + 0.05f * t, 0.22f + 0.58f * t, 0.3f - 0.1f * t, alpha * t};
}

/// A length said shortly: 0.42 m, 1.5 m, 12 m.
std::string metres(float m) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2g m", static_cast<double>(m));
    return text;
}

size_t countOf(const std::vector<uint8_t>& mask) {
    size_t n = 0;
    for (const uint8_t m : mask) n += m ? 1 : 0;
    return n;
}

/// Whether `picker` picks in `geo` -- made to, when only the points moved.
bool picks(ElementPicker& picker, const GeometryPtr& geo) {
    if (picker.geometry() == geo) return true;
    // Painted, not moved: the same tree still does.
    if (picker.fits(*geo)) {
        picker.adopt(geo);
        return true;
    }
    // Moved, not remade -- sculpted, dragged: the tree's boxes made again.
    return picker.refit(geo);
}

}  // namespace

// --- what is picked -----------------------------------------------------------------------

PickView SimWorkspace::pickView(const ViewCamera& cam) {
    PickView v;
    v.eye = cam.eye;
    v.forward = cam.forward;
    v.right = cam.right;
    v.up = cam.up;
    v.tanHalfFov = cam.tanHalfFov;
    v.x = cam.lo.x;
    v.y = cam.lo.y;
    v.width = cam.size.x;
    v.height = cam.size.y;
    return v;
}

const ElementPicker* SimWorkspace::picker(bool wait) {
    const GeometryPtr& geo = renderer_.geometry();
    if (!geo) return nullptr;
    wait = wait || synchronous_;
    // A new tree is made on a thread of its own -- a million faces' takes a
    // second. Once it is done it takes the place of the one there was,
    // unless that one picks in what is shown and it does not.
    auto take = [&](std::chrono::milliseconds patience) {
        if (!pickerMaking_.valid() || pickerMaking_.wait_for(patience) != std::future_status::ready) return;
        std::unique_ptr<ElementPicker> made = pickerMaking_.get();
        if (picks(*made, geo) || !picker_ || !picks(*picker_, geo)) picker_ = std::move(made);
    };
    auto make = [&] {
        pickerMaking_ = std::async(std::launch::async, [geo] { return std::make_unique<ElementPicker>(geo); });
    };
    take(std::chrono::milliseconds(0));
    if (picker_ && picks(*picker_, geo)) {
        // Refitted again and again, its boxes swell: a new tree is made
        // meanwhile, this one picking till it is done.
        if (picker_->swell() >= kMostSwell && !pickerMaking_.valid()) make();
        return picker_.get();
    }
    // Another topology: nothing is picked till its tree is made -- the old
    // one let go, unless a stroke goes on over it (paintTool). One being
    // made -- of another geometry, or of this one -- is let finish first.
    if (!stroking_) picker_.reset();
    if (pickerMaking_.valid()) {
        if (!wait) return nullptr;
        pickerMaking_.wait();
        take(std::chrono::milliseconds(0));
        if (picker_ && picks(*picker_, geo)) return picker_.get();
    }
    make();
    if (wait) pickerMaking_.wait();
    take(kPickerWait);
    return picker_ && picker_->geometry() == geo ? picker_.get() : nullptr;
}

void SimWorkspace::checkElements() {
    // What is picked is of the geometry shown as it was: of other size --
    // points deleted, another grid -- and it is gone. The same, it stays:
    // the node after it, an Edit undone, show the same points.
    const GeometryPtr& geo = renderer_.geometry();
    const bool any = !picked_.mask.empty() || !picked_.edges.empty();
    const bool gone = geo && (geo->pointCount() != picked_.points || geo->primitiveCount() != picked_.primitives ||
                              (elements_ == Elements::Vertices && !picked_.mask.empty() &&
                               picked_.mask.size() != geo->vertexCount()));
    if (any && gone) {
        picked_.mask.clear();
        picked_.edges.clear();
        ++picked_.revision;
        hoverElement_ = -1;
    }
    picked_.node = net_.displayed();
    if (paint_ && !brushNode() && !stroking_) {
        // The node painted into is no longer shown: the brush is put away.
        paint_ = false;
        paintMade_ = false;
        grabbing_ = false;
    }
}

size_t SimWorkspace::elementCount() const {
    return elements_ == Elements::Edges ? picked_.edges.size() : countOf(picked_.mask);
}

std::string SimWorkspace::elementPattern() const {
    // Asked many times a frame: found again only when what is picked changed.
    if (patternRevision_ != picked_.revision || patternElements_ != elements_) {
        patternCache_ = elements_ == Elements::Edges ? edgePatternOf(picked_.edges) : patternOf(picked_.mask);
        patternRevision_ = picked_.revision;
        patternElements_ = elements_;
    }
    return patternCache_;
}

AttrClass SimWorkspace::elementAttrClass() const {
    return elements_ == Elements::Primitives ? AttrClass::Primitive
           : elements_ == Elements::Vertices ? AttrClass::Vertex
                                             : AttrClass::Point;
}

int SimWorkspace::elementClass() const {
    return elements_ == Elements::Primitives ? 1 : elements_ == Elements::Vertices ? 2 : 0;
}

std::vector<uint8_t> SimWorkspace::elementPoints(const Geometry& geo) const {
    switch (elements_) {
        case Elements::Points: {
            std::vector<uint8_t> out = picked_.mask;
            out.resize(geo.pointCount(), 0);
            return out;
        }
        case Elements::Primitives: return pointsOfPrimitives(geo, picked_.mask);
        case Elements::Vertices: return pointsOfVertices(geo, picked_.mask);
        case Elements::Edges: {
            std::vector<uint8_t> out(geo.pointCount(), 0);
            for (const Edge& e : picked_.edges) {
                if (e.first < out.size()) out[e.first] = 1;
                if (e.second < out.size()) out[e.second] = 1;
            }
            return out;
        }
        default: return std::vector<uint8_t>(geo.pointCount(), 0);
    }
}

bool SimWorkspace::elementCenter(Vec3& center) const {
    const GeometryPtr& geo = renderer_.geometry();
    if (!geo || elementCount() == 0) return false;
    const std::vector<uint8_t> points = elementPoints(*geo);
    const auto P = geo->positions();
    Vec3 sum;
    size_t n = 0;
    for (size_t i = 0; i < points.size() && i < P.size(); ++i) {
        if (!points[i]) continue;
        sum += P[i];
        ++n;
    }
    if (n == 0) return false;
    center = sum * (1.0f / static_cast<float>(n));
    // What rounding left of a 0 -- the middle of a sphere -- is 0.
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(center[k]) < 1e-6f) center[k] = 0.0f;
    }
    return true;
}

void SimWorkspace::setElements(Elements mode) {
    if (mode == elements_) return;
    if (gizmo_.dragging()) return;
    const GeometryPtr& geo = renderer_.geometry();
    // What was picked, as the points it is -- and as the primitives, for
    // edges and vertices; the vertices, for primitives.
    std::vector<uint8_t> points, prims, corners;
    const bool had = geo && elementCount() > 0 && picked_.points == geo->pointCount() &&
                     picked_.primitives == geo->primitiveCount();
    if (had) {
        points = elementPoints(*geo);
        if (elements_ == Elements::Primitives) prims = picked_.mask;
        if (elements_ == Elements::Vertices) corners = picked_.mask;
    }
    elements_ = mode;
    picked_.mask.clear();
    picked_.edges.clear();
    ++picked_.revision;
    hoverElement_ = -1;
    hoverGeometry_ = nullptr;
    if (mode == Elements::Objects) return;
    if (paint_) setPaint(false);
    if (!geo) {
        setMessage(std::string("Nothing to pick ") + kindOf(mode, true) +
                       " of: display a geometry node (the flag at its right end, or R on it)",
                   true);
        return;
    }
    picked_.node = net_.displayed();
    picked_.points = geo->pointCount();
    picked_.primitives = geo->primitiveCount();
    if (!had) {
        // Nothing was picked and an Edit is shown: what it moves -- its
        // handle goes on with it.
        const sim::Node* n = net_.node(picked_.node);
        const std::string pattern = n && n->type == "edit" && !n->bypass ? net_.text(n->id, "group") : std::string();
        const int cls = n ? static_cast<int>(net_.value(n->id, "class")) : 0;
        if (pattern.empty()) return;
        if (mode == Elements::Points && cls == 0) {
            picked_.mask = selectElements(*geo, AttrClass::Point, pattern);
        } else if (mode == Elements::Primitives && cls == 1) {
            picked_.mask = selectElements(*geo, AttrClass::Primitive, pattern);
        } else if (mode == Elements::Vertices && cls == 2) {
            picked_.mask = selectElements(*geo, AttrClass::Vertex, pattern);
        } else if (mode == Elements::Edges && cls == 0) {
            const std::vector<Edge> all = edgesOf(*geo);
            picked_.edges = selectEdges(all, pattern);
        }
        ++picked_.revision;
        return;
    }
    switch (mode) {
        case Elements::Points: picked_.mask = points; break;
        case Elements::Primitives:
            picked_.mask = !corners.empty() ? primitivesOfVertices(*geo, corners) : primitivesOfPoints(*geo, points);
            break;
        case Elements::Vertices:
            picked_.mask = !prims.empty() ? verticesOfPrimitives(*geo, prims) : verticesOfPoints(*geo, points);
            break;
        case Elements::Edges:
            if (!prims.empty()) {
                // The sides of the primitives.
                for (size_t p = 0; p < prims.size() && p < geo->primitiveCount(); ++p) {
                    if (!prims[p]) continue;
                    const auto pts = geo->primitivePoints(p);
                    const size_t m = pts.size();
                    const size_t sides = m < 2 ? 0 : geo->primitiveClosed(p) ? m : m - 1;
                    for (size_t k = 0; k < sides; ++k) {
                        const uint32_t a = pts[k], b = pts[(k + 1) % m];
                        if (a != b) picked_.edges.emplace_back(std::min(a, b), std::max(a, b));
                    }
                }
                std::sort(picked_.edges.begin(), picked_.edges.end());
                picked_.edges.erase(std::unique(picked_.edges.begin(), picked_.edges.end()), picked_.edges.end());
            } else {
                // The edges between the points.
                for (const Edge& e : edgesOf(*geo)) {
                    if (e.first < points.size() && e.second < points.size() && points[e.first] && points[e.second]) {
                        picked_.edges.push_back(e);
                    }
                }
            }
            break;
        default: break;
    }
}

int32_t SimWorkspace::elementAt(const ViewCamera& cam, ImVec2 mouse, bool wait) {
    if (elements_ == Elements::Objects) return -1;
    const ElementPicker* p = picker(wait);
    if (!p) return -1;
    const PickView v = pickView(cam);
    switch (elements_) {
        case Elements::Points: return p->point(v, mouse.x, mouse.y, reach(), pickHidden_);
        case Elements::Edges: return p->edge(v, mouse.x, mouse.y, reach(), pickHidden_);
        case Elements::Primitives: return p->primitive(v, mouse.x, mouse.y, reach(), pickHidden_);
        case Elements::Vertices: return p->vertex(v, mouse.x, mouse.y, reach(), pickHidden_);
        default: return -1;
    }
}

void SimWorkspace::clickElements(const ViewCamera& cam, ImVec2 mouse, bool add, bool remove) {
    const GeometryPtr geo = renderer_.geometry();
    if (!geo || elements_ == Elements::Objects) return;
    const int32_t e = elementAt(cam, mouse, true);
    if (picked_.points != geo->pointCount() || picked_.primitives != geo->primitiveCount()) {
        picked_ = Picked{net_.displayed(), geo->pointCount(), geo->primitiveCount(), {}, {}, picked_.revision};
    }
    if (!add && !remove) {
        picked_.mask.clear();
        picked_.edges.clear();
    }
    if (elements_ == Elements::Edges) {
        if (e >= 0 && picker_ && static_cast<size_t>(e) < picker_->edges().size()) {
            const Edge edge = picker_->edges()[static_cast<size_t>(e)];
            auto at = std::lower_bound(picked_.edges.begin(), picked_.edges.end(), edge);
            const bool in = at != picked_.edges.end() && *at == edge;
            if (remove && in) picked_.edges.erase(at);
            else if (!remove && !in) picked_.edges.insert(at, edge);
        }
    } else {
        const size_t n = geo->elementCount(elementAttrClass());
        picked_.mask.resize(n, 0);
        if (e >= 0 && static_cast<size_t>(e) < n) picked_.mask[static_cast<size_t>(e)] = remove ? 0 : 1;
    }
    ++picked_.revision;
}

void SimWorkspace::regionElements(const ViewCamera& cam, const ScreenRegion& region, bool add, bool remove) {
    const GeometryPtr geo = renderer_.geometry();
    const ElementPicker* p = elements_ == Elements::Objects ? nullptr : picker(true);
    if (!geo || !p) return;
    if (picked_.points != geo->pointCount() || picked_.primitives != geo->primitiveCount()) {
        picked_ = Picked{net_.displayed(), geo->pointCount(), geo->primitiveCount(), {}, {}, picked_.revision};
    }
    const PickView v = pickView(cam);
    if (elements_ == Elements::Edges) {
        const std::vector<uint8_t> in = p->edgesIn(v, region, pickHidden_);
        std::vector<Edge> out;
        for (size_t i = 0; i < in.size(); ++i) {
            const Edge& e = p->edges()[i];
            const bool was = std::binary_search(picked_.edges.begin(), picked_.edges.end(), e);
            const bool now = remove ? was && !in[i] : add ? was || in[i] : in[i] != 0;
            if (now) out.push_back(e);
        }
        picked_.edges = std::move(out);
    } else {
        const std::vector<uint8_t> in = elements_ == Elements::Points     ? p->pointsIn(v, region, pickHidden_)
                                        : elements_ == Elements::Vertices ? p->verticesIn(v, region, pickHidden_)
                                                                          : p->primitivesIn(v, region, pickHidden_);
        picked_.mask.resize(in.size(), 0);
        for (size_t i = 0; i < in.size(); ++i) {
            const bool was = picked_.mask[i] != 0;
            picked_.mask[i] = (remove ? was && !in[i] : add ? was || in[i] : in[i] != 0) ? 1 : 0;
        }
    }
    ++picked_.revision;
}

void SimWorkspace::setPickStyle(PickStyle style) {
    if (boxing_ || brushing_) return;
    pickStyle_ = style;
    hoverElement_ = -1;
    hoverGeometry_ = nullptr;
    switch (style) {
        case PickStyle::Box: setMessage("Picking with a box: a drag picks what it holds (S: a lasso)"); break;
        case PickStyle::Lasso: setMessage("Picking with a lasso: draw round what to pick (S: a brush)"); break;
        case PickStyle::Brush:
            setMessage("Picking with a brush: what it goes over is picked -- Shift adds, Ctrl takes away; [ ] its size "
                       "(S: a box)");
            break;
    }
}

void SimWorkspace::setPickHidden(bool on) {
    pickHidden_ = on;
    hoverElement_ = -1;
    hoverGeometry_ = nullptr;
    setMessage(on ? "What the surface hides is picked too -- drawn faint (H: only what is seen)"
                  : "Only what is seen is picked (H: what is hidden too)");
}

float SimWorkspace::pickBrushRadius() const { return theme::px(pickBrush_ > 0.0f ? pickBrush_ : 24.0f); }

void SimWorkspace::pickBrushTool(ImDrawList* d, const ViewCamera& cam, bool overView, bool pressed) {
    const ImGuiIO& io = ImGui::GetIO();
    if (paint_ || pickStyle_ != PickStyle::Brush || elements_ == Elements::Objects) {
        brushing_ = false;
        return;
    }
    const float r = pickBrushRadius();
    const ImVec2 m = io.MousePos;
    // The press: a dab -- in place of what was picked, or added, or taken
    // away; then its way from frame to frame.
    if (pressed && overView && !gizmoOwnsMouse_ && !pressTurns_ && !brushing_) {
        brushing_ = true;
        brushRemoves_ = io.KeyCtrl;
        brushBefore_ = picked_;
        brushFrom_ = m;
        regionElements(cam, ScreenRegion::brush(m.x, m.y, m.x, m.y, r), io.KeyShift, brushRemoves_);
    } else if (brushing_ && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (m.x != brushFrom_.x || m.y != brushFrom_.y)) {
        regionElements(cam, ScreenRegion::brush(brushFrom_.x, brushFrom_.y, m.x, m.y, r), !brushRemoves_, brushRemoves_);
        brushFrom_ = m;
    }
    if (brushing_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) brushing_ = false;
    if (!overView && !brushing_) return;
    // Its ring, where the mouse is.
    const bool removes = brushing_ ? brushRemoves_ : io.KeyCtrl;
    const ImU32 col = removes ? IM_COL32(120, 190, 255, 230) : IM_COL32(255, 205, 80, 230);
    d->AddCircle(m, r, IM_COL32(0, 0, 0, 140), 48, theme::px(3.0f));
    d->AddCircle(m, r, col, 48, theme::px(1.5f));
    if (brushing_) d->AddCircleFilled(m, r, removes ? IM_COL32(120, 190, 255, 24) : IM_COL32(255, 200, 60, 24), 48);
}

// --- soft selection ---------------------------------------------------------------------------

int SimWorkspace::pickedEdit() const {
    if (elements_ == Elements::Objects || elementCount() == 0) return 0;
    const int shown = net_.displayed();
    const sim::Node* n = net_.node(shown);
    const bool ours = n && n->type == "edit" && !n->bypass && net_.text(shown, "group") == elementPattern() &&
                      static_cast<int>(net_.value(shown, "class")) == elementClass();
    return ours ? shown : 0;
}

SimWorkspace::Soft SimWorkspace::softNow() const {
    Soft s;
    s.on = soft_;
    s.radius = softRadius_;
    if (s.radius <= 0.0f) {
        // Not set yet: a share of the geometry's size, said shortly.
        Vec3 lo, hi;
        const float size = renderer_.geometryBounds(lo, hi) ? length(hi - lo) : 3.0f;
        const float r = std::max(0.15f * size, 1e-3f);
        const float step = std::pow(10.0f, std::floor(std::log10(r)) - 1.0f);
        s.radius = std::round(r / step) * step;
    }
    s.metric = softMetric_;
    s.falloff = softFalloff_;
    // The shown Edit of what is picked: its own.
    if (const int e = pickedEdit()) {
        const float frame = static_cast<float>(current_);
        const float r = net_.valueAt(e, "soft", frame)[0];
        s.on = r > 0.0f;
        if (s.on) s.radius = r;
        s.metric = static_cast<int>(net_.valueAt(e, "metric", frame)[0]);
        s.falloff = static_cast<int>(net_.valueAt(e, "falloff", frame)[0]);
    }
    return s;
}

void SimWorkspace::setSoft(bool on) {
    const Soft now = softNow();
    soft_ = on;
    softRadius_ = now.radius;
    if (const int e = pickedEdit()) net_.setParamAt(e, "soft", static_cast<float>(current_), {on ? softRadius_ : 0.0f, 0.0f, 0.0f});
    setMessage(on ? "Soft selection, " + metres(softRadius_) +
                        ": a drag takes the points round along, less the further they are -- [ ] or the wheel while "
                        "dragging: the radius; O: off"
                  : std::string("Soft selection off: a drag moves only what is picked"));
}

void SimWorkspace::setSoftRadius(float radius) {
    softRadius_ = std::clamp(radius, 1e-3f, 1000.0f);
    const int e = pickedEdit();
    if (e && softNow().on) net_.setParamAt(e, "soft", static_cast<float>(current_), {softRadius_, 0.0f, 0.0f});
}

void SimWorkspace::setSoftMetric(int metric) {
    softMetric_ = metric;
    if (const int e = pickedEdit()) net_.setParam(e, "metric", {static_cast<float>(metric), 0.0f, 0.0f});
}

void SimWorkspace::setSoftFalloff(int falloff) {
    softFalloff_ = falloff;
    if (const int e = pickedEdit()) net_.setParam(e, "falloff", {static_cast<float>(falloff), 0.0f, 0.0f});
}

int SimWorkspace::softBaseNode() const {
    if (!editingElements() || paint_) return 0;
    const int e = pickedEdit();
    if (!e || !softNow().on) return 0;
    const std::vector<sim::Link> in = net_.linksInto(e, "geometry");
    return in.empty() ? 0 : in.front().from;
}

const std::vector<float>& SimWorkspace::softShares() {
    const GeometryPtr& geo = renderer_.geometry();
    const Soft s = softNow();
    if (!geo || !s.on || !editingElements() || paint_ || elementCount() == 0 || picked_.points != geo->pointCount() ||
        picked_.primitives != geo->primitiveCount()) {
        softShares_.clear();
        softKey_.clear();
        return softShares_;
    }
    // Of the geometry the shown Edit moves, when it is shown: as it was
    // before the drag, as the Edit reckons them.
    const int base = softBaseNode();
    const bool before = base && softBase_ && softBaseNode_ == base && softBase_->pointCount() == geo->pointCount() &&
                        softBase_->primitiveCount() == geo->primitiveCount();
    const GeometryPtr& of = before ? softBase_ : geo;
    const Mirror mirror = symmetryNow();
    char key[200];
    std::snprintf(key, sizeof key, "%p %llu %d %g %d %d %d", static_cast<const void*>(of.get()),
                  static_cast<unsigned long long>(picked_.revision), static_cast<int>(elements_), static_cast<double>(s.radius),
                  s.metric, s.falloff, static_cast<int>(mirror));
    if (key != softKey_) {
        softKey_ = key;
        // Mirrored: the images of what is picked take their share too, as
        // the Edit reckons them.
        const std::vector<uint8_t> chosen = withMirror(*of, elementPoints(*geo), mirror);
        softShares_ = softWeights(*of, chosen, s.radius, s.metric == 1 ? SoftDistance::Surface : SoftDistance::Space,
                                  static_cast<Falloff>(std::clamp(s.falloff, 0, 4)));
    }
    return softShares_;
}

Mirror SimWorkspace::symmetryNow() const {
    if (const int e = pickedEdit()) {
        return static_cast<Mirror>(std::clamp(static_cast<int>(net_.valueAt(e, "symmetry", static_cast<float>(current_))[0]), 0, 3));
    }
    return static_cast<Mirror>(std::clamp(symmetry_, 0, 3));
}

void SimWorkspace::setSymmetry(int axis) {
    symmetry_ = std::clamp(axis, 0, 3);
    if (const int e = pickedEdit()) net_.setParam(e, "symmetry", {static_cast<float>(symmetry_), 0.0f, 0.0f});
    static const char* names[4] = {"off", "X", "Y", "Z"};
    setMessage(symmetry_ == 0 ? std::string("Symmetry off")
                              : std::string("Symmetry ") + names[symmetry_] +
                                    ": edits and brushes mirrored across the plane through the origin -- M: the next axis");
}

void SimWorkspace::drawMirrorPlane(ImDrawList* d, const ViewCamera& cam) {
    const Mirror m = symmetryNow();
    const int axis = mirrorAxis(m);
    Vec3 lo, hi;
    if (axis < 0 || !renderer_.geometryBounds(lo, hi)) return;
    // Across the box round the geometry, a little past it, through the origin.
    const Vec3 pad = (hi - lo) * 0.08f + Vec3(0.05f);
    lo -= pad;
    hi += pad;
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    Vec3 corner[4];
    for (int k = 0; k < 4; ++k) {
        corner[k][axis] = 0.0f;
        corner[k][u] = (k == 1 || k == 2) ? hi[u] : lo[u];
        corner[k][v] = k >= 2 ? hi[v] : lo[v];
    }
    ImVec2 at[4];
    for (int k = 0; k < 4; ++k) {
        if (!cam.toScreen(corner[k], at[k])) return;
    }
    const ImU32 col = IM_COL32(190, 140, 255, 170);
    d->AddPolyline(at, 4, IM_COL32(0, 0, 0, 90), ImDrawFlags_Closed, theme::px(2.5f));
    d->AddPolyline(at, 4, col, ImDrawFlags_Closed, theme::px(1.2f));
    static const char* names[4] = {"", "mirror X", "mirror Y", "mirror Z"};
    d->AddText(ImVec2(at[2].x + 1.0f, at[2].y + 1.0f), IM_COL32(0, 0, 0, 180), names[static_cast<int>(m)]);
    d->AddText(at[2], col, names[static_cast<int>(m)]);
}

void SimWorkspace::drawSoftRing(ImDrawList* d, const ViewCamera& cam, const Vec3& center) {
    const Soft s = softNow();
    if (!s.on || elementCount() == 0) return;
    ImVec2 at;
    if (!cam.toScreen(center, at)) return;
    // As big as the radius looks where the handle is.
    const float r = s.radius / std::max(cam.pixel(center), 1e-9f);
    if (r < 2.0f || r > 20000.0f) return;
    const ImU32 col = IM_COL32(255, 150, 90, 200);
    d->AddCircle(at, r, IM_COL32(0, 0, 0, 110), 96, theme::px(2.5f));
    d->AddCircle(at, r, col, 96, theme::px(1.2f));
    const std::string label = "soft " + metres(s.radius) + (s.metric == 1 ? ", along the surface" : "");
    const ImVec2 o(at.x + r * 0.7071f + theme::px(4.0f), at.y - r * 0.7071f - ImGui::GetFontSize());
    d->AddText(ImVec2(o.x + 1.0f, o.y + 1.0f), IM_COL32(0, 0, 0, 200), label.c_str());
    d->AddText(o, col, label.c_str());
}

void SimWorkspace::selectAllElements(bool invert) {
    const GeometryPtr geo = renderer_.geometry();
    if (!geo || elements_ == Elements::Objects) return;
    if (picked_.points != geo->pointCount() || picked_.primitives != geo->primitiveCount()) {
        picked_ = Picked{net_.displayed(), geo->pointCount(), geo->primitiveCount(), {}, {}, picked_.revision};
    }
    if (elements_ == Elements::Edges) {
        const std::vector<Edge> all = edgesOf(*geo);
        std::vector<Edge> out;
        for (const Edge& e : all) {
            if (!invert || !std::binary_search(picked_.edges.begin(), picked_.edges.end(), e)) out.push_back(e);
        }
        picked_.edges = std::move(out);
    } else {
        const size_t n = geo->elementCount(elementAttrClass());
        picked_.mask.resize(n, 0);
        for (uint8_t& m : picked_.mask) m = invert ? (m ? 0 : 1) : 1;
    }
    ++picked_.revision;
}

// --- the marks ---------------------------------------------------------------------------

const std::vector<Vec3>& SimWorkspace::pointNormals(const GeometryPtr& geo) {
    // The same points and faces as the geometry they were found for -- kept,
    // so that its buffers are not another's: the same normals.
    const Geometry& g = *geo;
    const bool same = normalsGeometry_ && normalsGeometry_->positions().data() == g.positions().data() &&
                      normalsGeometry_->vertexPoints().data() == g.vertexPoints().data() &&
                      normalsGeometry_->pointCount() == g.pointCount() &&
                      normalsGeometry_->primitiveCount() == g.primitiveCount();
    if (same) return normals_;
    normalsGeometry_ = geo;
    const auto P = g.positions();
    normals_.assign(g.pointCount(), Vec3());
    for (size_t p = 0; p < g.primitiveCount(); ++p) {
        const auto pts = g.primitivePoints(p);
        if (pts.size() < 3 || !g.primitiveClosed(p)) continue;
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            if (pts[0] >= P.size() || pts[k] >= P.size() || pts[k + 1] >= P.size()) continue;
            const Vec3 n = cross(P[pts[k]] - P[pts[0]], P[pts[k + 1]] - P[pts[0]]);
            for (const uint32_t q : {pts[0], pts[k], pts[k + 1]}) normals_[q] += n;
        }
    }
    return normals_;
}

const std::vector<Vec3>& SimWorkspace::primitiveNormals(const GeometryPtr& geo) {
    const Geometry& g = *geo;
    const bool same = primNormalsGeometry_ && primNormalsGeometry_->positions().data() == g.positions().data() &&
                      primNormalsGeometry_->vertexPoints().data() == g.vertexPoints().data() &&
                      primNormalsGeometry_->pointCount() == g.pointCount() &&
                      primNormalsGeometry_->primitiveCount() == g.primitiveCount();
    if (same) return primNormals_;
    primNormalsGeometry_ = geo;
    const auto P = g.positions();
    primNormals_.assign(g.primitiveCount(), Vec3());
    for (size_t p = 0; p < g.primitiveCount(); ++p) {
        const auto pts = g.primitivePoints(p);
        if (pts.size() < 3 || !g.primitiveClosed(p)) continue;
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            if (pts[0] >= P.size() || pts[k] >= P.size() || pts[k + 1] >= P.size()) continue;
            primNormals_[p] += cross(P[pts[k]] - P[pts[0]], P[pts[k + 1]] - P[pts[0]]);
        }
    }
    return primNormals_;
}

void SimWorkspace::updateOverlay() {
    const GeometryPtr& geo = renderer_.geometry();
    const bool on = editingElements() && geo != nullptr;
    // Picking what is hidden too, the marks it hides are drawn faint.
    const float faint = on && pickHidden_ && !paint_ ? 0.3f : 0.0f;
    if (faint != overlayHidden_) {
        overlayHidden_ = faint;
        renderer_.setOverlayHidden(faint);
        viewDirty_ = true;
    }
    const int painted = paint_ ? paintNode() : 0;
    const std::string attribute = painted ? net_.text(painted, "name") : std::string();
    const void* shown = geo.get();
    char key[256];

    // The geometry's own marks -- its wire, its points, the paint: made
    // again only when it changes. Sculpting, the surface alone: no wire
    // over what is shaped.
    const bool marked = on && !sculpting();
    // The wire and the corners are where the picker has them: drawn once
    // it is made.
    const ElementPicker* marker = marked ? picker() : nullptr;
    std::snprintf(key, sizeof key, "%d %d %p %d %s %d", marked ? 1 : 0, static_cast<int>(elements_), shown, painted,
                  attribute.c_str(), marker ? 1 : 0);
    if (key != overlayKey_[0]) {
        overlayKey_[0] = key;
        gl::Overlay o;
        if (marked) {
            const Geometry& g = *geo;
            const auto P = g.positions();
            // The wire: every edge, where there are not too many.
            if (marker && marker->edges().size() <= kMostMarks) {
                for (const Edge& e : marker->edges()) o.line(P[e.first], P[e.second], kWire);
            }
            if (painted) {
                // The paint: each corner in the colour of its point's value.
                const AttributeArray* a = g.points().find(attribute);
                std::vector<float> value(g.pointCount(), 0.0f);
                if (a && a->type() == AttrType::Float) {
                    const auto v = a->read<float>();
                    std::copy(v.begin(), v.end(), value.begin());
                } else if (a && a->type() == AttrType::Int) {
                    const auto v = a->read<int32_t>();
                    for (size_t i = 0; i < value.size(); ++i) value[i] = static_cast<float>(v[i]);
                }
                float most = 1.0f;
                for (const float x : value) most = std::max(most, std::fabs(x));
                for (float& x : value) x /= most;
                size_t faces = 0;
                for (size_t p = 0; p < g.primitiveCount(); ++p) {
                    const auto pts = g.primitivePoints(p);
                    if (pts.size() < 3 || !g.primitiveClosed(p)) continue;
                    for (size_t k = 1; k + 1 < pts.size(); ++k) {
                        o.face(P[pts[0]], P[pts[k]], P[pts[k + 1]], paintColor(value[pts[0]]), paintColor(value[pts[k]]),
                               paintColor(value[pts[k + 1]]));
                        ++faces;
                    }
                }
                if (faces == 0 && g.pointCount() <= kMostMarks) {
                    for (size_t i = 0; i < P.size(); ++i) o.dot(P[i], paintColor(value[i]), theme::px(6.0f));
                }
            } else if (elements_ == Elements::Points && P.size() <= kMostMarks) {
                // The points, each over the surface's normal there.
                const std::vector<Vec3>& normals = pointNormals(geo);
                for (size_t i = 0; i < P.size(); ++i) o.dot(P[i], kPoint, theme::px(5.0f), normals[i]);
            } else if (elements_ == Elements::Vertices && marker && g.vertexCount() <= kMostMarks) {
                // The corners, each a little inside its polygon, over its face.
                const std::vector<Vec3>& normals = primitiveNormals(geo);
                for (size_t v = 0; v < g.vertexCount(); ++v) {
                    o.dot(marker->vertexMark(v), kCorner, theme::px(6.0f), normals[marker->vertexPrimitive(v)]);
                }
            }
        }
        renderer_.setOverlay(o, 0);
        viewDirty_ = true;
    }

    // What is picked: a layer of its own, made again as that changes --
    // with every move of the brush that picks; with soft selection, the
    // share of a drag the points round take, over the surface.
    const bool marks = on && !painted;
    const std::vector<float>& shares = marks ? softShares() : softShares_;
    const ElementPicker* corners = marks && elements_ == Elements::Vertices ? picker() : nullptr;
    std::snprintf(key, sizeof key, "%d %d %p %llu %s %d", marks ? 1 : 0, static_cast<int>(elements_), shown,
                  static_cast<unsigned long long>(picked_.revision), marks ? softKey_.c_str() : "", corners ? 1 : 0);
    if (key != overlayKey_[1]) {
        overlayKey_[1] = key;
        gl::Overlay o;
        const bool mine = geo && picked_.points == geo->pointCount() && picked_.primitives == geo->primitiveCount();
        if (marks && mine && shares.size() == geo->pointCount()) {
            const Geometry& g = *geo;
            const auto P = g.positions();
            for (size_t p = 0; p < g.primitiveCount(); ++p) {
                const auto pts = g.primitivePoints(p);
                if (pts.size() < 3 || !g.primitiveClosed(p)) continue;
                for (size_t k = 1; k + 1 < pts.size(); ++k) {
                    const float a = shares[pts[0]], b = shares[pts[k]], c = shares[pts[k + 1]];
                    if (a <= 0.0f && b <= 0.0f && c <= 0.0f) continue;
                    o.face(P[pts[0]], P[pts[k]], P[pts[k + 1]], softColor(a, 0.5f), softColor(b, 0.5f), softColor(c, 0.5f));
                }
            }
            if (P.size() <= kMostMarks) {
                // The points that go part of the way.
                const std::vector<Vec3>& normals = pointNormals(geo);
                for (size_t i = 0; i < P.size(); ++i) {
                    if (shares[i] > 0.0f && shares[i] < 1.0f) {
                        const Vec4 c = softColor(shares[i], 1.0f);
                        o.dot(P[i], Vec4(c.x, c.y, c.z, 0.95f), theme::px(5.0f), normals[i]);
                    }
                }
            }
        }
        if (marks && mine) {
            const Geometry& g = *geo;
            const auto P = g.positions();
            if (elements_ == Elements::Points) {
                const std::vector<Vec3>& normals = pointNormals(geo);
                for (size_t i = 0; i < picked_.mask.size() && i < P.size(); ++i) {
                    if (picked_.mask[i]) o.dot(P[i], kPicked, theme::px(8.0f), normals[i]);
                }
            } else if (elements_ == Elements::Edges) {
                for (const Edge& e : picked_.edges) {
                    if (e.first < P.size() && e.second < P.size()) o.wideLine(P[e.first], P[e.second], kPicked, theme::px(3.0f));
                }
            } else if (elements_ == Elements::Vertices) {
                if (corners && picked_.mask.size() == g.vertexCount()) {
                    const std::vector<Vec3>& normals = primitiveNormals(geo);
                    for (size_t v = 0; v < picked_.mask.size(); ++v) {
                        if (picked_.mask[v]) {
                            o.dot(corners->vertexMark(v), kPicked, theme::px(8.0f), normals[corners->vertexPrimitive(v)]);
                        }
                    }
                }
            } else if (elements_ == Elements::Primitives) {
                for (size_t p = 0; p < picked_.mask.size() && p < g.primitiveCount(); ++p) {
                    if (!picked_.mask[p]) continue;
                    const auto pts = g.primitivePoints(p);
                    const bool face = pts.size() >= 3 && g.primitiveClosed(p);
                    if (face) {
                        for (size_t k = 1; k + 1 < pts.size(); ++k) o.face(P[pts[0]], P[pts[k]], P[pts[k + 1]], kPickedFace);
                    }
                    const size_t sides = pts.size() < 2 ? 0 : face ? pts.size() : pts.size() - 1;
                    for (size_t k = 0; k < sides; ++k) {
                        o.wideLine(P[pts[k]], P[pts[(k + 1) % pts.size()]], kPicked, theme::px(face ? 2.0f : 3.0f));
                    }
                }
            }
        }
        renderer_.setOverlay(o, 1);
        viewDirty_ = true;
    }

    // What is under the mouse: the last layer, made again as it moves.
    const bool hover = on && !paint_ && hoverElement_ >= 0;
    std::snprintf(key, sizeof key, "%d %d %p %d", hover ? 1 : 0, static_cast<int>(elements_), shown, hoverElement_);
    if (key != overlayKey_[2]) {
        overlayKey_[2] = key;
        gl::Overlay o;
        if (hover) {
            const Geometry& g = *geo;
            const auto P = g.positions();
            const size_t h = static_cast<size_t>(hoverElement_);
            if (elements_ == Elements::Points && h < P.size()) {
                o.dot(P[h], kHover, theme::px(10.0f), pointNormals(geo)[h]);
            } else if (elements_ == Elements::Edges && picker_ && h < picker_->edges().size()) {
                const Edge& e = picker_->edges()[h];
                o.wideLine(P[e.first], P[e.second], kHover, theme::px(3.0f));
            } else if (elements_ == Elements::Vertices && picker_ && h < g.vertexCount()) {
                // The corner, and the point it is the corner at.
                const Vec3 mark = picker_->vertexMark(h);
                const Vec3 normal = primitiveNormals(geo)[picker_->vertexPrimitive(h)];
                o.wideLine(P[g.vertexPoint(h)], mark, kHover, theme::px(2.0f));
                o.dot(mark, kHover, theme::px(10.0f), normal);
            } else if (elements_ == Elements::Primitives && h < g.primitiveCount()) {
                const auto pts = g.primitivePoints(h);
                const bool face = pts.size() >= 3 && g.primitiveClosed(h);
                if (face) {
                    for (size_t k = 1; k + 1 < pts.size(); ++k) o.face(P[pts[0]], P[pts[k]], P[pts[k + 1]], kHoverFace);
                }
                const size_t sides = pts.size() < 2 ? 0 : face ? pts.size() : pts.size() - 1;
                for (size_t k = 0; k < sides; ++k) o.wideLine(P[pts[k]], P[pts[(k + 1) % pts.size()]], kHover, theme::px(2.0f));
            }
        }
        renderer_.setOverlay(o, 2);
        viewDirty_ = true;
    }
}

// --- nodes put after the displayed one ----------------------------------------------------

int SimWorkspace::insertAfterDisplayed(const std::string& type) {
    const int from = net_.displayed();
    const sim::Node* n = net_.node(from);
    const sim::NodeType* t = n ? sim::findNodeType(n->type) : nullptr;
    if (!t || t->outputs.empty()) return 0;
    const std::string output = t->outputs.front().name;
    const float x = n->x + kStep, y = n->y;
    // Room for it: what comes after it moves along, if something is where it goes.
    bool taken = false;
    for (const sim::Node& o : net_.nodes()) {
        taken = taken || (o.id != from && std::fabs(o.x - x) < 0.6f * kStep && std::fabs(o.y - y) < 40.0f);
    }
    if (taken) {
        std::set<int> after;
        std::vector<int> todo{from};
        while (!todo.empty()) {
            const int k = todo.back();
            todo.pop_back();
            for (const sim::Link& l : net_.links()) {
                if (l.from == k && after.insert(l.to).second) todo.push_back(l.to);
            }
        }
        for (const int id : after) {
            if (sim::Node* m = net_.node(id)) m->x += kStep;
        }
    }
    std::vector<sim::Link> fed;
    for (const sim::Link& l : net_.links()) {
        if (l.from == from) fed.push_back(l);
    }
    const int id = net_.add(type, x, y);
    for (const sim::Link& l : fed) {
        net_.disconnect(l);
        net_.connect(id, "geometry", l.to, l.input);
    }
    net_.connect(from, output, id, "geometry");
    net_.setDisplay(id);
    canvas_.select(id);
    // What is picked is of its geometry now: the same points, moved or not.
    picked_.node = id;
    return id;
}

void SimWorkspace::extractNode(int id) {
    const std::vector<sim::Link> in = net_.linksInto(id, "geometry");
    std::vector<sim::Link> out;
    for (const sim::Link& l : net_.links()) {
        if (l.from == id) out.push_back(l);
    }
    const bool shown = net_.displayed() == id;
    net_.remove(id);
    if (in.empty()) return;
    for (const sim::Link& l : out) net_.connect(in.front().from, in.front().output, l.to, l.input);
    if (shown) {
        net_.setDisplay(in.front().from);
        if (picked_.node == id) picked_.node = in.front().from;
        canvas_.select(in.front().from);
    }
}

void SimWorkspace::groupElements() {
    const size_t n = elementCount();
    if (elements_ == Elements::Objects || n == 0) {
        setMessage("Pick points, edges or primitives to make a group of (2, 3, 4)", true);
        return;
    }
    const GeometryPtr geo = renderer_.geometry();
    // A name no group of the geometry has yet.
    std::string name;
    for (int k = 1;; ++k) {
        name = "group" + std::to_string(k);
        if (!geo || !geo->findGroup(name)) break;
    }
    const std::string pattern = elementPattern();
    const int cls = elementClass();
    const int id = insertAfterDisplayed("group");
    if (!id) return;
    net_.setText(id, "name", name);
    net_.setParam(id, "class", {static_cast<float>(cls), 0.0f, 0.0f});
    net_.setText(id, "pattern", pattern);
    setMessage("Group " + name + " of " + std::to_string(n) + " " + kindOf(elements_, n != 1) +
               (elements_ == Elements::Edges ? " -- a group of their points" : "") + ": rename it in its parameters");
}

void SimWorkspace::deleteElements() {
    const size_t n = elementCount();
    if (elements_ == Elements::Objects || n == 0) return;
    const std::string pattern = elementPattern();
    // Edges go with the primitives they are sides of; vertices are taken
    // out of their primitives.
    const int cls = elements_ == Elements::Points ? 0 : elements_ == Elements::Vertices ? 2 : 1;
    const int id = insertAfterDisplayed("blast");
    if (!id) return;
    net_.setText(id, "group", pattern);
    net_.setParam(id, "class", {static_cast<float>(cls), 0.0f, 0.0f});
    picked_.mask.clear();
    picked_.edges.clear();
    ++picked_.revision;
    setMessage("Deleted " + std::to_string(n) + " " + kindOf(elements_, n != 1) +
               (elements_ == Elements::Edges      ? " and the primitives on them"
                : elements_ == Elements::Vertices ? ", the polygons going on through the rest of their corners"
                                                  : "") +
               ": a Blast node");
}

void SimWorkspace::dissolveElements() {
    const size_t n = elementCount();
    if ((elements_ != Elements::Edges && elements_ != Elements::Primitives) || n == 0) {
        setMessage("Pick edges or faces to dissolve (3, 4)", true);
        return;
    }
    const std::string pattern = elementPattern();
    const float cls = elements_ == Elements::Primitives ? 1.0f : 0.0f;
    const int id = insertAfterDisplayed("dissolve");
    if (!id) return;
    net_.setText(id, "group", pattern);
    net_.setParam(id, "class", {cls, 0.0f, 0.0f});
    picked_.mask.clear();
    picked_.edges.clear();
    ++picked_.revision;
    setMessage("Dissolved " + std::to_string(n) + " " + kindOf(elements_, n != 1) +
               (elements_ == Elements::Edges ? ", the faces on them made one" : ", made one face") + ": a Dissolve node");
}

// --- the handle on what is picked ---------------------------------------------------------

void SimWorkspace::elementGizmo(ImDrawList* d, const ViewCamera& cam, bool overView) {
    const ImGuiIO& io = ImGui::GetIO();
    Vec3 center;
    const bool any = !paint_ && tool_ != GizmoMode::Select && elementCenter(center);
    // Nothing picked, a PolyExtrude shown: its own handle.
    if (!any && !editNode_ && !editPending_ && extrudeGizmo(d, cam, overView)) return;
    if (!any && !gizmo_.dragging()) return;
    if (gizmo_.dragging() && (paint_ || tool_ != gizmo_.drag().mode)) {
        // The tool changed under the drag: what it did stays.
        gizmo_.cancel();
        editNode_ = 0;
        return;
    }
    // The Edit node the handle sets: the one shown, when it is an Edit of
    // what is picked -- turned by it, so are the handle's axes.
    const std::string pattern = elementPattern();
    const int shown = net_.displayed();
    const sim::Node* n = net_.node(shown);
    const float frame = static_cast<float>(current_);
    const bool ours = n && n->type == "edit" && !n->bypass && net_.text(shown, "group") == pattern &&
                      static_cast<int>(net_.value(shown, "class")) == elementClass();
    const Vec3 turned = gizmo_.dragging() ? editR0_ : ours ? v3(net_.valueAt(shown, "r", frame)) : Vec3();
    // Sizes are along the Edit's own axes: scaling is always in them.
    const sim::Rotation axes = localAxes_ || tool_ == GizmoMode::Scale ? sim::Rotation::fromEuler(turned) : sim::Rotation();
    // Dragged, the middle is where the drag took it: the geometry cooks behind.
    Vec3 pivot = center;
    if (gizmo_.dragging()) pivot = editCenter0_ + (tool_ == GizmoMode::Move ? gizmo_.drag().move : Vec3());
    // Shift and Ctrl held, a click picks -- adds, takes away -- even on the handle.
    const bool free = overView && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                      (gizmo_.dragging() || (!io.KeyShift && !io.KeyCtrl));
    gizmo_.update(d, cam, tool_, pivot, axes, free, snap_ != io.KeyCtrl);
    drawSoftRing(d, cam, pivot);
    if (gizmo_.began()) {
        // The Edit shown goes on; else one is made -- once the drag moves
        // something: a click on the handle makes none.
        editMade_ = false;
        editPending_ = !ours;
        editNode_ = ours ? shown : 0;
        editPattern_ = pattern;
        editClass_ = elementClass();
        editT0_ = ours ? v3(net_.valueAt(shown, "t", frame)) : Vec3();
        editR0_ = ours ? v3(net_.valueAt(shown, "r", frame)) : Vec3();
        editS0_ = ours ? v3(net_.valueAt(shown, "s", frame)) : Vec3(1.0f, 1.0f, 1.0f);
        editP0_ = ours ? v3(net_.valueAt(shown, "p", frame)) : center;
        editCenter0_ = center;
        gizmoOwnsMouse_ = true;
    }
    if (gizmo_.dragging() || gizmo_.ended()) applyElementDrag(gizmo_.drag());
    if (gizmo_.ended()) {
        editNode_ = 0;
        editMade_ = editPending_ = false;
        recompile();
    }
}

void SimWorkspace::applyElementDrag(const GizmoDrag& drag) {
    if (editPending_ && !editNode_) {
        const bool still = drag.mode == GizmoMode::Move     ? drag.move == Vec3()
                           : drag.mode == GizmoMode::Rotate ? drag.degrees == 0.0f
                           : drag.mode == GizmoMode::Scale  ? drag.scale == Vec3(1.0f, 1.0f, 1.0f)
                                                            : true;
        if (still) return;
        const Soft soft = softNow();  // the viewport's: no Edit of what is picked is shown yet
        madeBefore_ = net_.save();
        editNode_ = insertAfterDisplayed("edit");
        editPending_ = false;
        editMade_ = editNode_ != 0;
        if (!editNode_) return;
        net_.setText(editNode_, "group", editPattern_);
        net_.setParam(editNode_, "class", {static_cast<float>(editClass_), 0.0f, 0.0f});
        net_.setParam(editNode_, "p", pv(editP0_));
        net_.setParam(editNode_, "soft", {soft.on ? soft.radius : 0.0f, 0.0f, 0.0f});
        net_.setParam(editNode_, "metric", {static_cast<float>(soft.metric), 0.0f, 0.0f});
        net_.setParam(editNode_, "falloff", {static_cast<float>(soft.falloff), 0.0f, 0.0f});
        net_.setParam(editNode_, "symmetry", {static_cast<float>(symmetry_), 0.0f, 0.0f});
    }
    if (!net_.node(editNode_)) return;
    // What the Edit did when the drag began, then what the drag does about
    // the handle's middle then: again what an Edit does (sim::EditTransform).
    sim::EditTransform e;
    e.t = editT0_;
    e.r = editR0_;
    e.s = editS0_;
    e.p = editP0_;
    switch (drag.mode) {
        case GizmoMode::Move: e = e.moved(drag.move); break;
        case GizmoMode::Rotate: e = e.turned(sim::Rotation::about(drag.axis, drag.degrees), editCenter0_); break;
        case GizmoMode::Scale: e = e.sized(drag.scale, editCenter0_); break;
        case GizmoMode::Select: break;
    }
    const float frame = static_cast<float>(current_);
    net_.setParamAt(editNode_, "t", frame, pv(e.t));
    net_.setParamAt(editNode_, "r", frame, pv(e.r));
    net_.setParamAt(editNode_, "s", frame, pv(e.s));
}

void SimWorkspace::restoreElementDrag() {
    editPending_ = false;
    if (!net_.node(editNode_)) {
        editNode_ = 0;
        return;
    }
    if (editMade_) {
        // Made by this drag: the network as it was before it.
        restore(madeBefore_);
    } else {
        const float frame = static_cast<float>(current_);
        net_.setParamAt(editNode_, "t", frame, pv(editT0_));
        net_.setParamAt(editNode_, "r", frame, pv(editR0_));
        net_.setParamAt(editNode_, "s", frame, pv(editS0_));
    }
    editNode_ = 0;
    editMade_ = false;
}

// --- the brush ------------------------------------------------------------------------------

int SimWorkspace::paintNode() const {
    const sim::Node* n = net_.node(net_.displayed());
    return n && n->type == "attribute_paint" && !n->bypass ? n->id : 0;
}

int SimWorkspace::sculptNode() const {
    const sim::Node* n = net_.node(net_.displayed());
    return n && n->type == "sculpt" && !n->bypass ? n->id : 0;
}

SculptDab::Tool SimWorkspace::sculptTool() const {
    const int node = sculptNode();
    if (!node) return SculptDab::Tool::Push;
    if (ImGui::GetIO().KeyShift) return SculptDab::Tool::Smooth;  // as in every sculpting program
    const int tool = static_cast<int>(net_.valueAt(node, "tool", static_cast<float>(current_))[0]);
    return static_cast<SculptDab::Tool>(std::clamp(tool, 0, 3));
}

float SimWorkspace::dyntopoDetail(float radius) const {
    const int node = sculptNode();
    const float frame = static_cast<float>(current_);
    if (!node || net_.valueAt(node, "dyntopo", frame)[0] == 0.0f) return 0.0f;
    if (net_.valueAt(node, "detailmode", frame)[0] >= 0.5f) return net_.valueAt(node, "detailsize", frame)[0];
    return net_.valueAt(node, "detail", frame)[0] * radius;
}

void SimWorkspace::setDyntopo(bool on) {
    const int node = sculptNode();
    if (!node) return;
    net_.setParam(node, "dyntopo", {on ? 1.0f : 0.0f, 0.0f, 0.0f});
    setMessage(on ? std::string("Dyntopo: the mesh made finer under the brush as it goes, coarser where it is finer "
                                "than it needs -- triangles; Detail in the node -- Ctrl+D: off")
                  : std::string("Dyntopo off: the brush moves the points there are"));
}

void SimWorkspace::setPaint(bool on) {
    // On what is shown when it takes a brush; else a new Attribute Paint.
    setBrush(on, sculptNode() ? "sculpt" : "attribute_paint");
}

void SimWorkspace::setSculpt(bool on) {
    if (on && paint_ && !sculptNode()) setBrush(false, "");  // an Attribute Paint's brush put down first
    setBrush(on, "sculpt");
}

void SimWorkspace::setBrush(bool on, const char* type) {
    if (on == paint_) return;
    const bool sculpt = std::string(type) == "sculpt";
    if (on) {
        if (!renderer_.geometry() || !net_.node(net_.displayed())) {
            setMessage(std::string("Nothing to ") + (sculpt ? "sculpt" : "paint on") +
                           ": display a geometry node first (the flag at its right end, or R on it)",
                       true);
            return;
        }
        paint_ = true;
        paintMade_ = false;
        paintNode_ = sculpt ? sculptNode() : brushNode();
        if (!paintNode_) {
            madeBefore_ = net_.save();
            paintNode_ = insertAfterDisplayed(type);
            paintMade_ = paintNode_ != 0;
            // A brush as big as a twelfth of what is sculpted, said shortly.
            Vec3 lo, hi;
            if (paintNode_ && sculpt && renderer_.geometryBounds(lo, hi) && length(hi - lo) > 1e-6f) {
                const float r = length(hi - lo) / 12.0f;
                const float step = std::pow(10.0f, std::floor(std::log10(r)) - 1.0f);
                net_.setParam(paintNode_, "radius", {std::round(r / step) * step, 0.0f, 0.0f});
            }
            madeAfter_ = net_.save();
        }
        if (!paintNode_) {
            paint_ = false;
            return;
        }
        canvas_.select(paintNode_);
        hoverElement_ = -1;
        if (sculptNode()) {
            setMessage("Sculpting " + net_.node(paintNode_)->name +
                       ": drag on the geometry -- Shift smooths, Ctrl pulls in; the tool in its parameters or the right "
                       "click; [ ] or Shift+wheel: the size; U again: done");
        } else {
            setMessage("Painting " + net_.text(paintNode_, "name") +
                       ": drag on the geometry -- Ctrl paints the Erase Value; [ ] or Shift+wheel: the size; P again: done");
        }
        return;
    }
    paint_ = false;
    stroking_ = false;
    grabbing_ = false;
    // Made by P or U and never used: taken out again -- the network as it
    // was, when nothing else changed since.
    if (paintMade_ && net_.node(paintNode_) && net_.text(paintNode_, "strokes").empty()) {
        if (net_.save() == madeAfter_) restore(madeBefore_);
        else extractNode(paintNode_);
    }
    paintMade_ = false;
    paintNode_ = 0;
}

void SimWorkspace::scaleBrush(float factor) {
    const int node = brushNode();
    if (!node) return;
    const float frame = static_cast<float>(current_);
    const float r = std::clamp(net_.valueAt(node, "radius", frame)[0] * factor, 1e-3f, 100.0f);
    net_.setParamAt(node, "radius", frame, {r, 0.0f, 0.0f});
}

void SimWorkspace::paintTool(ImDrawList* d, const ViewCamera& cam, bool overView) {
    const ImGuiIO& io = ImGui::GetIO();
    const int node = brushNode();
    const bool sculpt = node && node == sculptNode();
    brushHit_ = false;
    if (!paint_ || !node) {
        stroking_ = grabbing_ = false;
        return;
    }
    const float frame = static_cast<float>(current_);
    const float radius = std::max(net_.valueAt(node, "radius", frame)[0], 1e-4f);
    const SculptDab::Tool tool = sculpt ? sculptTool() : SculptDab::Tool::Push;
    // The brush is where the ray under the mouse meets the surface.
    const bool inside = io.MousePos.x >= cam.lo.x && io.MousePos.y >= cam.lo.y && io.MousePos.x < cam.lo.x + cam.size.x &&
                        io.MousePos.y < cam.lo.y + cam.size.y;
    if (overView || (stroking_ && inside)) {
        // The press waits for the picker. While a stroke goes on -- its
        // faces remade by dyntopo -- the one of a moment ago does, not
        // waited for: the brush stays on the surface.
        const ElementPicker* p = picker(overView && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
        if (!p && stroking_) p = picker_.get();
        if (p) {
            Vec3 o, dir;
            cam.ray(io.MousePos, o, dir);
            float t = 0.0f;
            Vec3 normal;
            if (p->raycast(o, dir, t, &normal) >= 0) {
                brushHit_ = true;
                brushAt_ = o + dir * t;
                brushNormal_ = normal;
            }
        }
    }
    auto append = [&](const std::string& text) {
        std::string strokes = net_.text(node, "strokes");
        if (!strokes.empty()) strokes += "; ";
        strokes += text;
        net_.setText(node, "strokes", strokes);
    };
    // Symmetry: each dab and its mirror image -- weaker where they overlap.
    const Mirror mirror = static_cast<Mirror>(std::clamp(symmetry_, 0, 3));
    // A dab: of paint -- or of sculpting, with the tool in the hand.
    auto dab = [&](const Vec3& at) {
        const float strength = net_.valueAt(node, "strength", frame)[0];
        if (sculpt) {
            SculptDab s;
            s.tool = tool;
            s.at = at;
            s.normal = normalize(brushNormal_);
            s.radius = radius;
            s.strength = tool == SculptDab::Tool::Push && io.KeyCtrl ? -strength : strength;
            if (mirror == Mirror::None) {
                append(sculptText(s));
            } else {
                const auto both = mirroredDabs(s, mirror);
                append(sculptText(both[0]) + "; " + sculptText(both[1]));
            }
            return;
        }
        const float value = net_.valueAt(node, io.KeyCtrl ? "erase" : "value", frame)[0];
        auto paintText = [&](const Vec3& where, float share) {
            char text[160];
            std::snprintf(text, sizeof text, "%.4f %.4f %.4f %.4g %.4g %.3g", static_cast<double>(where.x),
                          static_cast<double>(where.y), static_cast<double>(where.z), static_cast<double>(radius),
                          static_cast<double>(value), static_cast<double>(strength * share));
            return std::string(text);
        };
        if (mirror == Mirror::None) {
            append(paintText(at, 1.0f));
        } else {
            const Vec3 image = mirrored(at, mirror);
            const float share = std::clamp(length(image - at) / (2.0f * radius), 0.5f, 1.0f);
            append(paintText(at, share) + "; " + paintText(image, share));
        }
    };
    // The grab as text: and its mirror image.
    auto grabText = [&](const SculptDab& g) {
        if (mirror == Mirror::None) return sculptText(g);
        const auto both = mirroredDabs(g, mirror);
        return sculptText(both[0]) + "; " + sculptText(both[1]);
    };
    // A stroke begins with the press, on the surface or off it: it works
    // where the brush is on it. Off it and back, it begins afresh there --
    // not across what is between.
    const bool turns = io.KeyAlt || ImGui::IsKeyDown(ImGuiKey_Space);
    if (!stroking_ && overView && !turns && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        stroking_ = true;
        strokeOn_ = false;
    }
    if (stroking_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (sculpt && tool == SculptDab::Tool::Grab) {
            // Grab: what is under the press goes with the mouse -- one dab,
            // its move set again as the mouse goes, in the plane of the
            // screen through where it took hold.
            if (!grabbing_ && brushHit_) {
                grabbing_ = true;
                grab_ = SculptDab{};
                grab_.tool = SculptDab::Tool::Grab;
                grab_.at = brushAt_;
                grab_.radius = radius;
                grabBefore_ = net_.text(node, "strokes");
                net_.setText(node, "strokes", grabBefore_ + (grabBefore_.empty() ? "" : "; ") + grabText(grab_));
            } else if (grabbing_) {
                Vec3 o, dir;
                cam.ray(io.MousePos, o, dir);
                const float along = dot(dir, cam.forward);
                if (std::fabs(along) > 1e-6f) {
                    const Vec3 to = o + dir * (dot(grab_.at - o, cam.forward) / along);
                    if (!(to - grab_.at == grab_.move)) {
                        grab_.move = to - grab_.at;
                        net_.setText(node, "strokes", grabBefore_ + (grabBefore_.empty() ? "" : "; ") + grabText(grab_));
                    }
                }
            }
        } else if (!brushHit_) {
            strokeOn_ = false;
        } else if (!strokeOn_) {
            dab(brushAt_);
            lastDab_ = brushAt_;
            strokeOn_ = true;
        } else {
            // Dabs along the stroke, a quarter of the brush apart.
            const float spacing = radius * 0.25f;
            const float far = length(brushAt_ - lastDab_);
            if (far >= spacing) {
                const int steps = std::min(static_cast<int>(far / spacing), 64);
                const Vec3 from = lastDab_;
                for (int k = 1; k <= steps; ++k) {
                    dab(from + (brushAt_ - from) * (static_cast<float>(k) / static_cast<float>(steps)));
                }
                lastDab_ = brushAt_;
            }
        }
    }
    if (stroking_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) stroking_ = strokeOn_ = grabbing_ = false;
    // The brush: a ring as big as it is, lying on the surface -- where the
    // grab took hold, moved -- and what it does.
    const bool held = grabbing_;
    if (!brushHit_ && !held) return;
    const Vec3 at = held ? grab_.at + grab_.move : brushAt_;
    const Vec3 nrm = normalize(held ? cam.forward * -1.0f : brushNormal_);
    const Vec3 helper = std::fabs(nrm.y) < 0.9f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 u = normalize(cross(nrm, helper)), v = cross(nrm, u);
    ImVec2 ring[64];
    int count = 0;
    for (int i = 0; i < 64; ++i) {
        const float a = 6.28318531f * static_cast<float>(i) / 64.0f;
        ImVec2 sp;
        if (cam.toScreen(at + (u * std::cos(a) + v * std::sin(a)) * radius, sp)) ring[count++] = sp;
    }
    const bool erase = io.KeyCtrl;
    ImU32 col = erase ? IM_COL32(120, 190, 255, 235) : IM_COL32(255, 236, 200, 235);
    std::string label;
    if (sculpt) {
        static const char* names[4] = {"Push", "Smooth", "Grab", "Flatten"};
        static const ImU32 colors[4] = {IM_COL32(255, 196, 120, 235), IM_COL32(140, 230, 150, 235),
                                        IM_COL32(255, 150, 90, 235), IM_COL32(200, 160, 255, 235)};
        const int k = static_cast<int>(tool);
        col = tool == SculptDab::Tool::Push && erase ? IM_COL32(120, 190, 255, 235) : colors[k];
        label = tool == SculptDab::Tool::Push && erase ? "Pull" : names[k];
        label += " " + metres(radius);
        // With dyntopo, how long the edges under it are made -- Grab takes the mesh as it is.
        if (const float edge = dyntopoDetail(radius); edge > 0.0f && tool != SculptDab::Tool::Grab) {
            label += ", edges " + metres(edge);
        }
    } else {
        char text[96];
        std::snprintf(text, sizeof text, "%s %g", net_.text(node, "name").c_str(),
                      static_cast<double>(net_.valueAt(node, erase ? "erase" : "value", frame)[0]));
        label = text;
    }
    if (count > 2) {
        d->AddPolyline(ring, count, IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, theme::px(3.0f));
        d->AddPolyline(ring, count, col, ImDrawFlags_Closed, theme::px(1.5f));
    }
    ImVec2 mid;
    if (cam.toScreen(at, mid)) d->AddCircleFilled(mid, theme::px(2.0f), col);
    if (held) {
        ImVec2 from;
        if (cam.toScreen(grab_.at, from) && cam.toScreen(at, mid)) d->AddLine(from, mid, col, theme::px(1.2f));
    }
    const ImVec2 o(io.MousePos.x + theme::px(14.0f), io.MousePos.y + theme::px(10.0f));
    d->AddText(ImVec2(o.x + 1.0f, o.y + 1.0f), IM_COL32(0, 0, 0, 180), label.c_str());
    d->AddText(o, col, label.c_str());
}

// --- nodes on what is picked ----------------------------------------------------------------

std::string SimWorkspace::patternFor(AttrClass cls) const {
    const GeometryPtr& geo = renderer_.geometry();
    if (!geo || elementCount() == 0) return {};
    // Edges name their points, or the primitives they are sides of, alike.
    if (elements_ == Elements::Edges) return edgePatternOf(picked_.edges);
    if (elements_ == Elements::Vertices) {
        if (cls == AttrClass::Vertex) return patternOf(picked_.mask);
        return cls == AttrClass::Primitive ? patternOf(primitivesOfVertices(*geo, picked_.mask))
                                           : patternOf(pointsOfVertices(*geo, picked_.mask));
    }
    const bool prims = elements_ == Elements::Primitives;
    if ((cls == AttrClass::Primitive) == prims) return patternOf(picked_.mask);
    return cls == AttrClass::Primitive ? patternOf(primitivesOfPoints(*geo, picked_.mask))
                                       : patternOf(pointsOfPrimitives(*geo, picked_.mask));
}

void SimWorkspace::applyToPicked(const std::string& type) {
    const sim::NodeType* t = sim::findNodeType(type);
    if (!t || !net_.node(net_.displayed())) {
        setMessage("Display a geometry node first: Tab puts a node after it", true);
        return;
    }
    const size_t n = elementCount();
    // The class the node works on: its own -- a Point or Primitive
    // Wrangle, PolyExtrude's faces -- or, where it has a choice, ours.
    AttrClass cls = elements_ == Elements::Primitives ? AttrClass::Primitive : AttrClass::Point;
    if (elements_ == Elements::Vertices) {
        // Vertices where the node takes them -- a Group, an Edit, a Blast --
        // else their points.
        const sim::ParamDef* c = t->param("class");
        if (c && c->choices.size() > 2) cls = AttrClass::Vertex;
    }
    bool fixed = false;
    if (type == "polyextrude" || type == "primitive_wrangle") {
        cls = AttrClass::Primitive;
        fixed = true;
    } else if (type == "point_wrangle") {
        cls = AttrClass::Point;
        fixed = true;
    }
    const std::string pattern = n ? patternFor(cls) : std::string();
    const int id = insertAfterDisplayed(type);
    if (!id) return;
    if (!pattern.empty() && t->param("group")) net_.setText(id, "group", pattern);
    if (!fixed && t->param("class") && n) {
        net_.setParam(id, "class", {cls == AttrClass::Primitive ? 1.0f : cls == AttrClass::Vertex ? 2.0f : 0.0f, 0.0f, 0.0f});
    }
    const sim::Node* made = net_.node(id);
    setMessage(std::string(t->label) + " " + (made ? made->name : std::string()) +
               (pattern.empty() ? std::string(" after the shown node")
                                : " on " + std::to_string(n) + " " + kindOf(elements_, n != 1) + " picked"));
}

// --- the handle of a PolyExtrude -------------------------------------------------------------

bool SimWorkspace::extrudeGizmo(ImDrawList* d, const ViewCamera& cam, bool overView) {
    const int shown = net_.displayed();
    const sim::Node* n = net_.node(shown);
    const GeometryPtr& geo = renderer_.geometry();
    if (!n || n->type != "polyextrude" || n->bypass || !geo || paint_ || tool_ == GizmoMode::Select) return false;
    const float frame = static_cast<float>(current_);
    // Its faces moved: their middle, and which way they went.
    Vec3 center = extrudeCenter0_, normal = extrudeNormal0_;
    if (!gizmo_.dragging() || extrudeNode_ != shown) {
        const Group* front = geo->findGroup(net_.text(shown, "frontgroup"));
        if (!front || front->classOf() != AttrClass::Primitive || front->memberCount() == 0) return false;
        const auto P = geo->positions();
        Vec3 sum, dir;
        size_t count = 0;
        for (size_t p = 0; p < geo->primitiveCount(); ++p) {
            if (!front->contains(p)) continue;
            const auto pts = geo->primitivePoints(p);
            if (pts.size() < 3) continue;
            for (size_t k = 1; k + 1 < pts.size(); ++k) dir += cross(P[pts[k]] - P[pts[0]], P[pts[k + 1]] - P[pts[0]]);
            for (const uint32_t q : pts) {
                sum += P[q];
                ++count;
            }
        }
        if (count == 0 || length(dir) < 1e-12f) return false;
        center = sum * (1.0f / static_cast<float>(count));
        normal = normalize(dir);
    }
    // The handle's y along the normal: dragged, Distance grows by how far
    // along it the drag went.
    const Vec3 helper = std::fabs(normal.y) < 0.9f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    sim::Rotation axes;
    axes.y = normal;
    axes.x = normalize(cross(helper, normal));
    axes.z = cross(axes.x, axes.y);
    const float distance = net_.valueAt(shown, "distance", frame)[0];
    Vec3 pivot = center;
    if (gizmo_.dragging()) pivot = extrudeCenter0_ + extrudeNormal0_ * dot(gizmo_.drag().move, extrudeNormal0_);
    const ImGuiIO& io = ImGui::GetIO();
    const bool free = overView && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    gizmo_.update(d, cam, GizmoMode::Move, pivot, axes, free, snap_ != io.KeyCtrl);
    if (gizmo_.began()) {
        extrudeNode_ = shown;
        extrudeDistance0_ = distance;
        extrudeCenter0_ = center;
        extrudeNormal0_ = normal;
        gizmoOwnsMouse_ = true;
    }
    if ((gizmo_.dragging() || gizmo_.ended()) && extrudeNode_ == shown) {
        const float along = dot(gizmo_.drag().move, extrudeNormal0_);
        net_.setParamAt(shown, "distance", frame, {extrudeDistance0_ + along, 0.0f, 0.0f});
    }
    if (gizmo_.ended()) extrudeNode_ = 0;
    // What it is: said by the arrow.
    ImVec2 at;
    if (cam.toScreen(pivot + normal * (cam.pixel(pivot) * Gizmo::screenLength() * 1.1f), at)) {
        char text[64];
        std::snprintf(text, sizeof text, "Distance %.3g m", static_cast<double>(net_.valueAt(shown, "distance", frame)[0]));
        const ImVec2 o(at.x + theme::px(8.0f), at.y - theme::px(8.0f));
        d->AddText(ImVec2(o.x + 1.0f, o.y + 1.0f), IM_COL32(0, 0, 0, 200), text);
        d->AddText(o, IM_COL32(235, 236, 240, 235), text);
    }
    return true;
}

// --- the numbers of the elements -------------------------------------------------------------

void SimWorkspace::drawNumbers(ImDrawList* d, const ViewCamera& cam) {
    const GeometryPtr& geo = renderer_.geometry();
    if (!numbers_ || !editingElements() || !geo) return;
    // The middles, the marks and what is hidden are the picker's: no
    // numbers till it is made.
    const ElementPicker* p = picker();
    if (!p) return;
    // Primitives numbered at their middles, vertices at their marks, else points.
    const int kind = elements_ == Elements::Primitives ? 1 : elements_ == Elements::Vertices ? 2 : 0;
    // Those seen, found again when the view or the geometry changed.
    char key[200];
    std::snprintf(key, sizeof key, "%p %d %g %g %g %g %g %g %g %g", static_cast<const void*>(geo.get()), kind,
                  static_cast<double>(cam.eye.x), static_cast<double>(cam.eye.y), static_cast<double>(cam.eye.z),
                  static_cast<double>(cam.forward.x), static_cast<double>(cam.forward.y), static_cast<double>(cam.forward.z),
                  static_cast<double>(cam.size.x), static_cast<double>(cam.size.y));
    constexpr size_t kMost = 3000;
    if (key != numbersKey_) {
        numbersKey_ = key;
        numberAt_.clear();
        const size_t count = kind == 1 ? geo->primitiveCount() : kind == 2 ? geo->vertexCount() : geo->pointCount();
        const auto P = geo->positions();
        for (size_t i = 0; i < count && numberAt_.size() <= kMost; ++i) {
            const Vec3 at = kind == 0 ? P[i] : kind == 1 ? p->middle(i) : p->vertexMark(i);
            ImVec2 s;
            if (!cam.toScreen(at, s) || s.x < cam.lo.x || s.y < cam.lo.y || s.x > cam.lo.x + cam.size.x ||
                s.y > cam.lo.y + cam.size.y) {
                continue;
            }
            numberAt_.emplace_back(at, static_cast<uint32_t>(i));
        }
        // Too many on screen to read: none -- come nearer.
        if (numberAt_.size() > kMost) {
            numberAt_.clear();
            numbersKey_ += " many";
        } else {
            std::vector<std::pair<Vec3, uint32_t>> seen;
            for (const auto& [at, i] : numberAt_) {
                if (p->visible(cam.eye, at)) seen.emplace_back(at, i);
            }
            numberAt_ = std::move(seen);
        }
    }
    if (numbersKey_.size() >= 5 && numbersKey_.compare(numbersKey_.size() - 5, 5, " many") == 0) {
        const char* text = "Too many numbers to show: come nearer";
        d->AddText(ImVec2(cam.lo.x + theme::px(60.0f), cam.lo.y + cam.size.y - theme::px(60.0f)), theme::kTextDim, text);
        return;
    }
    const ImU32 col = kind == 1 ? IM_COL32(255, 214, 120, 235) : kind == 2 ? IM_COL32(150, 240, 160, 235)
                                                                           : IM_COL32(170, 220, 255, 235);
    char text[16];
    for (const auto& [at, i] : numberAt_) {
        ImVec2 s;
        if (!cam.toScreen(at, s)) continue;
        std::snprintf(text, sizeof text, "%u", i);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        // A primitive's on its middle; a point's or a vertex's beside it.
        const bool centred = kind == 1;
        const ImVec2 o(s.x + (centred ? -ts.x * 0.5f : theme::px(4.0f)), s.y - (centred ? ts.y * 0.5f : ts.y + theme::px(2.0f)));
        d->AddText(ImVec2(o.x + 1.0f, o.y + 1.0f), IM_COL32(0, 0, 0, 200), text);
        d->AddText(o, col, text);
    }
}

std::string SimWorkspace::elementStatus() const {
    if (!editingElements()) return {};
    const GeometryPtr& geo = renderer_.geometry();
    if (!geo) return "Nothing shown to pick in: display a geometry node (its flag, or R on it)";
    const float frame = static_cast<float>(current_);
    if (paint_ && sculptNode()) {
        const int node = sculptNode();
        static const char* names[4] = {"Push / Pull", "Smooth", "Grab", "Flatten"};
        const SculptDab::Tool tool = sculptTool();
        // The keys that change what this tool does: Ctrl only pulls a push in.
        const char* keys = tool == SculptDab::Tool::Push     ? "Shift smooths, Ctrl pulls in  \xc2\xb7  "
                           : tool == SculptDab::Tool::Smooth ? ""
                                                             : "Shift smooths  \xc2\xb7  ";
        const float radius = net_.valueAt(node, "radius", frame)[0];
        const float edge = dyntopoDetail(radius);
        return "Sculpting " + net_.node(node)->name + ": " + names[static_cast<int>(tool)] + ", radius " + metres(radius) +
               (edge > 0.0f ? ", dyntopo edges " + metres(edge) : std::string()) + "  \xc2\xb7  " + keys +
               "[ ] size  \xc2\xb7  Ctrl+D dyntopo  \xc2\xb7  U: done";
    }
    if (paint_) {
        const int node = paintNode();
        if (!node) return {};
        char text[200];
        std::snprintf(text, sizeof text, "Painting %s: %g, Ctrl %g  \xc2\xb7  radius %.3g m  \xc2\xb7  [ ] size  \xc2\xb7  P: done",
                      net_.text(node, "name").c_str(), static_cast<double>(net_.valueAt(node, "value", frame)[0]),
                      static_cast<double>(net_.valueAt(node, "erase", frame)[0]),
                      static_cast<double>(net_.valueAt(node, "radius", frame)[0]));
        return text;
    }
    const size_t n = elementCount();
    std::string text;
    if (hoverElement_ >= 0) {
        const size_t h = static_cast<size_t>(hoverElement_);
        if (elements_ == Elements::Edges && picker_ && h < picker_->edges().size()) {
            const Edge& e = picker_->edges()[h];
            text = "edge " + std::to_string(e.first) + "-" + std::to_string(e.second);
        } else if (elements_ == Elements::Vertices && picker_ && renderer_.geometry() && h < renderer_.geometry()->vertexCount()) {
            // As Houdini names it too: the primitive and which of its corners.
            const Geometry& g = *renderer_.geometry();
            const uint32_t prim = picker_->vertexPrimitive(h);
            text = "vertex " + std::to_string(h) + " (" + std::to_string(prim) + "v" +
                   std::to_string(h - g.primitiveVertexStart(prim)) + ", point " + std::to_string(g.vertexPoint(h)) + ")";
        } else {
            text = std::string(kindOf(elements_, false)) + " " + std::to_string(h);
        }
        if (n) text += "  \xc2\xb7  ";
    }
    if (n) {
        text += std::to_string(n) + " " + kindOf(elements_, n != 1) + " picked";
        const Soft soft = softNow();
        if (soft.on) text += "  \xc2\xb7  soft " + metres(soft.radius);
        if (hoverElement_ < 0) {
            text += "  \xc2\xb7  W E R move, turn, size  \xc2\xb7  Ctrl+G group  \xc2\xb7  Delete";
            if (elements_ == Elements::Edges || elements_ == Elements::Primitives) text += "  \xc2\xb7  Ctrl+X dissolve";
        }
    } else if (hoverElement_ < 0) {
        const std::string kind = kindOf(elements_, true);
        text = pickStyle_ == PickStyle::Brush ? "Paint over the " + kind + " to pick them"
               : pickStyle_ == PickStyle::Lasso ? "Click, or draw round the " + kind + " to pick"
                                                : "Click or drag a box to pick " + kind;
        text += "  \xc2\xb7  Shift adds, Ctrl takes away  \xc2\xb7  S: box, lasso, brush  \xc2\xb7  Alt+drag turns the view";
    }
    // First, what is not seen otherwise: the hints after it go first when
    // the viewport is narrow.
    if (pickHidden_) text = "Hidden too" + (text.empty() ? std::string() : "  \xc2\xb7  " + text);
    if (const Mirror m = symmetryNow(); m != Mirror::None) {
        static const char* names[4] = {"", "Mirror X", "Mirror Y", "Mirror Z"};
        text = names[static_cast<int>(m)] + (text.empty() ? std::string() : "  \xc2\xb7  " + text);
    }
    return text;
}

}  // namespace pg::editor
