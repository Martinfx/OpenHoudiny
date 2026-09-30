// Editing the displayed geometry in the viewport, as in Houdini: its
// points, edges and primitives picked with the mouse; what is picked moved,
// turned and sized by the handle, made a group, deleted; an attribute
// painted on with a brush. Each is a node put after the displayed one --
// Edit, Group, Blast, Attribute Paint -- whose parameters hold what the
// mouse did: the elements as a pattern (pg/core/Selection.h), the strokes as
// dabs. What cannot be seen is not picked: the geometry's own surface hides
// what is behind it (pg/core/Pick.h) -- unless asked for (H).
//
//   1 2 3 4            objects; points, edges, primitives
//   click, drag        pick one, or what a box holds: Shift adds, Ctrl takes away
//   S                  a drag draws a box, a lasso, or is a brush that picks what
//                      it goes over -- [ ] or Shift+wheel: its size
//   H                  what the surface hides is picked too, and drawn faint
//   Alt or Space held  the left button turns the view (middle: pan; right, wheel: zoom)
//   W E R              move, turn, size what is picked -- an Edit node; Q: no handle
//   Ctrl+G             a Group of it;   Delete, X: a Blast of it
//   Ctrl+A, Ctrl+I     all of them, the others;   Escape: none
//   P                  the brush: the Attribute Paint node's attribute, its Value
//                      -- Ctrl: its Erase Value; [ ] or Shift+wheel: its size
//
// The marks -- the wire, the points, what is picked and what is under the
// mouse, the paint -- are the renderer's overlay: hidden behind what is in
// front of them, as the geometry is.
#include "SimWorkspace.h"

#include <algorithm>
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

/// Past so many, the wire and the points are not drawn -- what is picked is.
constexpr size_t kMostMarks = 400000;
/// How far to the right of the node before it a node put after one goes.
constexpr float kStep = 230.0f;

/// Pixels from the mouse an element is still under it.
float reach() { return theme::px(7.0f); }

const char* kindOf(SimWorkspace::Elements e, bool many) {
    switch (e) {
        case SimWorkspace::Elements::Points: return many ? "points" : "point";
        case SimWorkspace::Elements::Edges: return many ? "edges" : "edge";
        case SimWorkspace::Elements::Primitives: return many ? "primitives" : "primitive";
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

size_t countOf(const std::vector<uint8_t>& mask) {
    size_t n = 0;
    for (const uint8_t m : mask) n += m ? 1 : 0;
    return n;
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

const ElementPicker* SimWorkspace::picker() {
    const GeometryPtr& geo = renderer_.geometry();
    if (!geo) return nullptr;
    if (picker_ && picker_->geometry() == geo) return picker_.get();
    // Painted, not moved: the same tree still does.
    if (picker_ && picker_->fits(*geo)) {
        picker_->adopt(geo);
        return picker_.get();
    }
    if (!picker_) picker_ = std::make_unique<ElementPicker>();
    picker_->build(geo);
    return picker_.get();
}

void SimWorkspace::checkElements() {
    // What is picked is of the geometry shown as it was: of other size --
    // points deleted, another grid -- and it is gone. The same, it stays:
    // the node after it, an Edit undone, show the same points.
    const GeometryPtr& geo = renderer_.geometry();
    const bool any = !picked_.mask.empty() || !picked_.edges.empty();
    const bool gone = geo && (geo->pointCount() != picked_.points || geo->primitiveCount() != picked_.primitives);
    if (any && gone) {
        picked_.mask.clear();
        picked_.edges.clear();
        ++picked_.revision;
        hoverElement_ = -1;
    }
    picked_.node = net_.displayed();
    if (paint_ && !paintNode() && !stroking_) {
        // The node painted into is no longer shown: the brush is put away.
        paint_ = false;
        paintMade_ = false;
    }
}

size_t SimWorkspace::elementCount() const {
    return elements_ == Elements::Edges ? picked_.edges.size() : countOf(picked_.mask);
}

std::string SimWorkspace::elementPattern() const {
    return elements_ == Elements::Edges ? edgePatternOf(picked_.edges) : patternOf(picked_.mask);
}

int SimWorkspace::elementClass() const { return elements_ == Elements::Primitives ? 1 : 0; }

std::vector<uint8_t> SimWorkspace::elementPoints(const Geometry& geo) const {
    switch (elements_) {
        case Elements::Points: {
            std::vector<uint8_t> out = picked_.mask;
            out.resize(geo.pointCount(), 0);
            return out;
        }
        case Elements::Primitives: return pointsOfPrimitives(geo, picked_.mask);
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
    // What was picked, as the points it is -- and as the primitives, for edges.
    std::vector<uint8_t> points, prims;
    const bool had = geo && elementCount() > 0 && picked_.points == geo->pointCount() &&
                     picked_.primitives == geo->primitiveCount();
    if (had) {
        points = elementPoints(*geo);
        if (elements_ == Elements::Primitives) prims = picked_.mask;
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
    if (!had) return;
    switch (mode) {
        case Elements::Points: picked_.mask = points; break;
        case Elements::Primitives: picked_.mask = primitivesOfPoints(*geo, points); break;
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

int32_t SimWorkspace::elementAt(const ViewCamera& cam, ImVec2 mouse) {
    if (elements_ == Elements::Objects) return -1;
    const ElementPicker* p = picker();
    if (!p) return -1;
    const PickView v = pickView(cam);
    switch (elements_) {
        case Elements::Points: return p->point(v, mouse.x, mouse.y, reach(), pickHidden_);
        case Elements::Edges: return p->edge(v, mouse.x, mouse.y, reach(), pickHidden_);
        case Elements::Primitives: return p->primitive(v, mouse.x, mouse.y, reach(), pickHidden_);
        default: return -1;
    }
}

void SimWorkspace::clickElements(const ViewCamera& cam, ImVec2 mouse, bool add, bool remove) {
    const GeometryPtr geo = renderer_.geometry();
    if (!geo || elements_ == Elements::Objects) return;
    const int32_t e = elementAt(cam, mouse);
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
        const size_t n = elements_ == Elements::Points ? geo->pointCount() : geo->primitiveCount();
        picked_.mask.resize(n, 0);
        if (e >= 0 && static_cast<size_t>(e) < n) picked_.mask[static_cast<size_t>(e)] = remove ? 0 : 1;
    }
    ++picked_.revision;
}

void SimWorkspace::regionElements(const ViewCamera& cam, const ScreenRegion& region, bool add, bool remove) {
    const GeometryPtr geo = renderer_.geometry();
    const ElementPicker* p = picker();
    if (!geo || !p || elements_ == Elements::Objects) return;
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
        const std::vector<uint8_t> in = elements_ == Elements::Points ? p->pointsIn(v, region, pickHidden_)
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
        const size_t n = elements_ == Elements::Points ? geo->pointCount() : geo->primitiveCount();
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
    // again only when it changes.
    std::snprintf(key, sizeof key, "%d %d %p %d %s", on ? 1 : 0, static_cast<int>(elements_), shown, painted,
                  attribute.c_str());
    if (key != overlayKey_[0]) {
        overlayKey_[0] = key;
        gl::Overlay o;
        if (on) {
            const Geometry& g = *geo;
            const auto P = g.positions();
            // The wire: every edge, where there are not too many.
            if (edgeGeometry_ != geo) {
                const bool same = edgeGeometry_ && edgeGeometry_->vertexPoints().data() == g.vertexPoints().data() &&
                                  edgeGeometry_->pointCount() == g.pointCount() &&
                                  edgeGeometry_->primitiveCount() == g.primitiveCount();
                if (!same) edges_ = edgesOf(g);
                edgeGeometry_ = geo;
            }
            if (edges_.size() <= kMostMarks) {
                for (const Edge& e : edges_) o.line(P[e.first], P[e.second], kWire);
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
            }
        }
        renderer_.setOverlay(o, 0);
        viewDirty_ = true;
    }

    // What is picked: a layer of its own, made again as that changes --
    // with every move of the brush that picks.
    const bool marks = on && !painted;
    std::snprintf(key, sizeof key, "%d %d %p %llu", marks ? 1 : 0, static_cast<int>(elements_), shown,
                  static_cast<unsigned long long>(picked_.revision));
    if (key != overlayKey_[1]) {
        overlayKey_[1] = key;
        gl::Overlay o;
        const bool mine = geo && picked_.points == geo->pointCount() && picked_.primitives == geo->primitiveCount();
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
    // Edges go with the primitives they are sides of.
    const int cls = elements_ == Elements::Points ? 0 : 1;
    const int id = insertAfterDisplayed("blast");
    if (!id) return;
    net_.setText(id, "group", pattern);
    net_.setParam(id, "class", {static_cast<float>(cls), 0.0f, 0.0f});
    picked_.mask.clear();
    picked_.edges.clear();
    ++picked_.revision;
    setMessage("Deleted " + std::to_string(n) + " " + kindOf(elements_, n != 1) +
               (elements_ == Elements::Edges ? " and the primitives on them" : "") + ": a Blast node");
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
        madeBefore_ = net_.save();
        editNode_ = insertAfterDisplayed("edit");
        editPending_ = false;
        editMade_ = editNode_ != 0;
        if (!editNode_) return;
        net_.setText(editNode_, "group", editPattern_);
        net_.setParam(editNode_, "class", {static_cast<float>(editClass_), 0.0f, 0.0f});
        net_.setParam(editNode_, "p", pv(editP0_));
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

void SimWorkspace::setPaint(bool on) {
    if (on == paint_) return;
    if (on) {
        if (!renderer_.geometry() || !net_.node(net_.displayed())) {
            setMessage("Nothing to paint on: display a geometry node first (the flag at its right end, or R on it)", true);
            return;
        }
        paint_ = true;
        paintMade_ = false;
        paintNode_ = paintNode();
        if (!paintNode_) {
            madeBefore_ = net_.save();
            paintNode_ = insertAfterDisplayed("attribute_paint");
            paintMade_ = paintNode_ != 0;
            madeAfter_ = net_.save();
        }
        if (!paintNode_) {
            paint_ = false;
            return;
        }
        canvas_.select(paintNode_);
        hoverElement_ = -1;
        setMessage("Painting " + net_.text(paintNode_, "name") +
                   ": drag on the geometry -- Ctrl paints the Erase Value; [ ] or Shift+wheel: the size; P again: done");
        return;
    }
    paint_ = false;
    stroking_ = false;
    // Made by P and never painted with: taken out again -- the network as it
    // was, when nothing else changed since.
    if (paintMade_ && net_.node(paintNode_) && net_.text(paintNode_, "strokes").empty()) {
        if (net_.save() == madeAfter_) restore(madeBefore_);
        else extractNode(paintNode_);
    }
    paintMade_ = false;
    paintNode_ = 0;
}

void SimWorkspace::scaleBrush(float factor) {
    const int node = paintNode();
    if (!node) return;
    const float frame = static_cast<float>(current_);
    const float r = std::clamp(net_.valueAt(node, "radius", frame)[0] * factor, 1e-3f, 100.0f);
    net_.setParamAt(node, "radius", frame, {r, 0.0f, 0.0f});
}

void SimWorkspace::paintTool(ImDrawList* d, const ViewCamera& cam, bool overView) {
    const ImGuiIO& io = ImGui::GetIO();
    const int node = paintNode();
    brushHit_ = false;
    if (!paint_ || !node) {
        stroking_ = false;
        return;
    }
    const float frame = static_cast<float>(current_);
    const float radius = std::max(net_.valueAt(node, "radius", frame)[0], 1e-4f);
    // The brush is where the ray under the mouse meets the surface.
    const bool inside = io.MousePos.x >= cam.lo.x && io.MousePos.y >= cam.lo.y && io.MousePos.x < cam.lo.x + cam.size.x &&
                        io.MousePos.y < cam.lo.y + cam.size.y;
    if (overView || (stroking_ && inside)) {
        if (const ElementPicker* p = picker()) {
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
    // Dabs along the stroke, a quarter of the brush apart.
    auto dab = [&](const Vec3& at) {
        const float value = net_.valueAt(node, io.KeyCtrl ? "erase" : "value", frame)[0];
        const float strength = net_.valueAt(node, "strength", frame)[0];
        char text[160];
        std::snprintf(text, sizeof text, "%.4f %.4f %.4f %.4g %.4g %.3g", static_cast<double>(at.x), static_cast<double>(at.y),
                      static_cast<double>(at.z), static_cast<double>(radius), static_cast<double>(value),
                      static_cast<double>(strength));
        std::string strokes = net_.text(node, "strokes");
        if (!strokes.empty()) strokes += "; ";
        strokes += text;
        net_.setText(node, "strokes", strokes);
    };
    // A stroke begins with the press, on the surface or off it: it paints
    // where the brush is on it. Off it and back, it begins afresh there --
    // not across what is between.
    const bool turns = io.KeyAlt || ImGui::IsKeyDown(ImGuiKey_Space);
    if (!stroking_ && overView && !turns && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        stroking_ = true;
        strokeOn_ = false;
    }
    if (stroking_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!brushHit_) {
            strokeOn_ = false;
        } else if (!strokeOn_) {
            dab(brushAt_);
            lastDab_ = brushAt_;
            strokeOn_ = true;
        } else {
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
    if (stroking_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) stroking_ = strokeOn_ = false;
    if (!brushHit_) return;
    // The brush: a ring as big as it is, lying on the surface, and what it lays on.
    const Vec3 nrm = normalize(brushNormal_);
    const Vec3 helper = std::fabs(nrm.y) < 0.9f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 u = normalize(cross(nrm, helper)), v = cross(nrm, u);
    ImVec2 ring[64];
    int count = 0;
    for (int i = 0; i < 64; ++i) {
        const float a = 6.28318531f * static_cast<float>(i) / 64.0f;
        ImVec2 s;
        if (cam.toScreen(brushAt_ + (u * std::cos(a) + v * std::sin(a)) * radius, s)) ring[count++] = s;
    }
    const bool erase = io.KeyCtrl;
    const ImU32 col = erase ? IM_COL32(120, 190, 255, 235) : IM_COL32(255, 236, 200, 235);
    if (count > 2) {
        d->AddPolyline(ring, count, IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, theme::px(3.0f));
        d->AddPolyline(ring, count, col, ImDrawFlags_Closed, theme::px(1.5f));
    }
    ImVec2 mid;
    if (cam.toScreen(brushAt_, mid)) d->AddCircleFilled(mid, theme::px(2.0f), col);
    char label[96];
    std::snprintf(label, sizeof label, "%s %g", net_.text(node, "name").c_str(),
                  static_cast<double>(net_.valueAt(node, erase ? "erase" : "value", frame)[0]));
    const ImVec2 at(io.MousePos.x + theme::px(14.0f), io.MousePos.y + theme::px(10.0f));
    d->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 180), label);
    d->AddText(at, col, label);
}

// --- nodes on what is picked ----------------------------------------------------------------

std::string SimWorkspace::patternFor(AttrClass cls) const {
    const GeometryPtr& geo = renderer_.geometry();
    if (!geo || elementCount() == 0) return {};
    // Edges name their points, or the primitives they are sides of, alike.
    if (elements_ == Elements::Edges) return edgePatternOf(picked_.edges);
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
        net_.setParam(id, "class", {cls == AttrClass::Primitive ? 1.0f : 0.0f, 0.0f, 0.0f});
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
    const bool prims = elements_ == Elements::Primitives;
    // Those seen, found again when the view or the geometry changed.
    char key[200];
    std::snprintf(key, sizeof key, "%p %d %g %g %g %g %g %g %g %g", static_cast<const void*>(geo.get()), prims ? 1 : 0,
                  static_cast<double>(cam.eye.x), static_cast<double>(cam.eye.y), static_cast<double>(cam.eye.z),
                  static_cast<double>(cam.forward.x), static_cast<double>(cam.forward.y), static_cast<double>(cam.forward.z),
                  static_cast<double>(cam.size.x), static_cast<double>(cam.size.y));
    constexpr size_t kMost = 3000;
    if (key != numbersKey_) {
        numbersKey_ = key;
        numberAt_.clear();
        const ElementPicker* p = picker();
        const size_t count = prims ? geo->primitiveCount() : geo->pointCount();
        const auto P = geo->positions();
        for (size_t i = 0; i < count && numberAt_.size() <= kMost; ++i) {
            const Vec3 at = prims ? (p ? p->middle(i) : Vec3()) : P[i];
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
        } else if (p) {
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
    const ImU32 col = prims ? IM_COL32(255, 214, 120, 235) : IM_COL32(170, 220, 255, 235);
    char text[16];
    for (const auto& [at, i] : numberAt_) {
        ImVec2 s;
        if (!cam.toScreen(at, s)) continue;
        std::snprintf(text, sizeof text, "%u", i);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 o(s.x + (prims ? -ts.x * 0.5f : theme::px(4.0f)), s.y - (prims ? ts.y * 0.5f : ts.y + theme::px(2.0f)));
        d->AddText(ImVec2(o.x + 1.0f, o.y + 1.0f), IM_COL32(0, 0, 0, 200), text);
        d->AddText(o, col, text);
    }
}

// --- what the viewport says -------------------------------------------------------------------

std::string SimWorkspace::elementStatus() const {
    if (!editingElements()) return {};
    const GeometryPtr& geo = renderer_.geometry();
    if (!geo) return "Nothing shown to pick in: display a geometry node (its flag, or R on it)";
    const float frame = static_cast<float>(current_);
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
        } else {
            text = std::string(kindOf(elements_, false)) + " " + std::to_string(h);
        }
        if (n) text += "  \xc2\xb7  ";
    }
    if (n) {
        text += std::to_string(n) + " " + kindOf(elements_, n != 1) + " picked";
        if (hoverElement_ < 0) text += "  \xc2\xb7  W E R move, turn, size  \xc2\xb7  Ctrl+G group  \xc2\xb7  Delete";
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
    return text;
}

}  // namespace pg::editor
