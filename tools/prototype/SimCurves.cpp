// The curve editor: the animated parameters of the selected nodes drawn as
// curves of their value over the frames, between the viewport and the
// timeline (View > Curve Editor). A curve is one channel -- a number, or one
// component of a vector -- sampled through the same evaluate() the solver
// reads, so what it draws is what the simulation gets: Smooth eases, Linear
// runs straight, Step holds.
//
// Keys are the diamonds on it: dragged, added where a curve is double
// clicked, deleted. A key holds every component of its parameter (Network.h),
// so dragging one up and down moves that component alone, while dragging it
// sideways carries the whole key -- the components of a vector keep to the
// same frames.
#include "SimWorkspace.h"

#include "Theme.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pg::editor {

using theme::Icon;

namespace {

/// Frames and values nearer than this are the same: keys are found by frame
/// (Network::setKey), and a span this thin would divide by nothing.
constexpr float kSame = 1e-4f;
/// How near a key must be, in pixels, to be picked by a click.
constexpr float kReach = 7.0f;
/// The least a graph spans: a flat curve still gets a band round it.
constexpr float kLeastSpan = 1e-3f;

/// The colour of component `c` of `channels` many: a vector's x, y and z red,
/// green and blue; a number its own, by where it comes.
ImU32 colorOf(int component, int channels, size_t which) {
    if (channels == 3) {
        const ImU32 axes[3] = {theme::kRed, theme::kGreen, theme::kBlue};
        return axes[component];
    }
    const ImU32 others[5] = {theme::kYellow, theme::kGreen, theme::kBlue, theme::kAccent, theme::kRed};
    return others[which % 5];
}

/// A step for the ruler's ticks that leaves at least `least` pixels between
/// them: 1, 2, 5, 10, 20, 50...
float tickStep(float perUnit, float least) {
    if (!(perUnit > 0.0f)) return 1.0f;
    float step = std::pow(10.0f, std::floor(std::log10(std::max(least / perUnit, 1e-9f))));
    for (const float c : {1.0f, 2.0f, 5.0f, 10.0f}) {
        step = c * step;
        if (step * perUnit >= least) break;
    }
    return step;
}

}  // namespace

float SimWorkspace::timelineHeight() const { return ImGui::GetFrameHeight() + theme::px(16.0f); }

std::vector<SimWorkspace::Curve> SimWorkspace::shownCurves() const {
    std::vector<Curve> out;
    // The selected nodes, in the order they were selected in, each of their
    // animated parameters in the order the node has them.
    for (const int id : canvas_.selection()) {
        const sim::Node* n = net_.node(id);
        if (!n) continue;
        for (const sim::ParamDef* p : net_.params(id)) {
            if (sim::isText(p->kind)) continue;
            const std::vector<sim::Key>* keys = net_.keys(id, p->name);
            if (!keys || keys->empty()) continue;
            const std::vector<std::string> channels = sim::Network::channels(*p);
            for (size_t c = 0; c < channels.size(); ++c) {
                // A channel an expression drives is not its keys': it would
                // draw a curve the simulation does not follow.
                if (!net_.expression(id, channels[c]).empty()) continue;
                Curve curve;
                curve.node = id;
                curve.def = p;
                curve.component = static_cast<int>(c);
                curve.label = n->name + "  " + channels[c];
                curve.color = colorOf(curve.component, static_cast<int>(channels.size()), out.size());
                out.push_back(std::move(curve));
            }
        }
    }
    return out;
}

void SimWorkspace::frameCurves(const std::vector<Curve>& curves) {
    curveFrom_ = 1.0f;
    curveTo_ = static_cast<float>(std::max(2, compiled_.frames));
    float lo = 0.0f, hi = 0.0f;
    bool any = false;
    for (const Curve& c : curves) {
        const std::vector<sim::Key>* keys = net_.keys(c.node, c.def->name);
        if (!keys) continue;
        for (const sim::Key& k : *keys) {
            const float v = k.value[static_cast<size_t>(c.component)];
            lo = any ? std::min(lo, v) : v;
            hi = any ? std::max(hi, v) : v;
            any = true;
            curveFrom_ = std::min(curveFrom_, k.frame);
            curveTo_ = std::max(curveTo_, k.frame);
        }
    }
    if (!any) {
        lo = 0.0f;
        hi = 1.0f;
    }
    // A tenth of the span round it, so the topmost and lowest keys are not
    // on the edge; a flat curve gets a band of its own.
    const float span = std::max(hi - lo, kLeastSpan);
    curveLo_ = lo - 0.1f * span;
    curveHi_ = hi + 0.1f * span;
}

void SimWorkspace::curves(ImVec2 size) {
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    const ImVec2 hi(lo.x + size.x, lo.y + size.y);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(lo, hi, theme::kCanvas);
    const std::vector<Curve> curves = shownCurves();

    // The ruler at the foot, the names down the left; the graph is the rest.
    const float ruler = ImGui::GetFontSize() + theme::px(6.0f);
    const float names = theme::px(130.0f);
    const ImVec2 glo(lo.x + names, lo.y + theme::px(4.0f));
    const ImVec2 ghi(hi.x - theme::px(8.0f), hi.y - ruler);
    const float gw = std::max(1.0f, ghi.x - glo.x), gh = std::max(1.0f, ghi.y - glo.y);

    if (curves.empty()) {
        const char* what = canvas_.selection().empty()
                               ? "Select a node to see its animated parameters here"
                               : "Nothing animated on what is selected: a key makes a curve (the diamond by a parameter)";
        const ImVec2 t = ImGui::CalcTextSize(what);
        ui::overlayText(d, ImVec2(std::floor((lo.x + hi.x - t.x) * 0.5f), std::floor((lo.y + hi.y - t.y) * 0.5f)),
                        theme::kTextFaint, what);
        curveFramed_ = false;
        curvePicked_.clear();
        ImGui::Dummy(size);
        return;
    }
    // Fitted to the curves when they change -- another node selected, a key
    // added -- unless the view has been moved since.
    uint64_t fit = 1469598103934665603ull;
    for (const Curve& c : curves) {
        fit = mixKey(fit, c.node);
        fit = mixKey(fit, c.component);
        fit = mixKey(fit, c.def);
    }
    fit = mixKey(fit, static_cast<uint64_t>(compiled_.frames));
    if (!curveFramed_ || fit != curveFit_) {
        frameCurves(curves);
        curveFit_ = fit;
        curveFramed_ = true;
    }
    const float frameSpan = std::max(curveTo_ - curveFrom_, kSame);
    const float valueSpan = std::max(curveHi_ - curveLo_, kLeastSpan);
    auto xOf = [&](float frame) { return glo.x + (frame - curveFrom_) / frameSpan * gw; };
    auto yOf = [&](float value) { return ghi.y - (value - curveLo_) / valueSpan * gh; };
    auto frameAt = [&](float x) { return curveFrom_ + (x - glo.x) / gw * frameSpan; };
    auto valueAt = [&](float y) { return curveLo_ + (ghi.y - y) / gh * valueSpan; };

    // The graph takes the mouse: clicks on keys and in space, drags, the
    // wheel. The ruler scrubs, as the timeline's track does.
    ImGui::SetCursorScreenPos(glo);
    ImGui::InvisibleButton("curves.graph", ImVec2(gw, gh),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    // What the graph did with the mouse, taken here: the ruler below is the
    // last item by the time the keys are handled.
    const bool overGraph = ImGui::IsItemHovered();
    const bool graphActive = ImGui::IsItemActive();
    const bool graphPressed = ImGui::IsItemActivated();
    const bool graphRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetCursorScreenPos(ImVec2(glo.x, ghi.y));
    ImGui::InvisibleButton("curves.ruler", ImVec2(gw, ruler));
    if (ImGui::IsItemActive()) {
        playing_ = false;
        current_ = std::clamp(static_cast<int>(std::lround(frameAt(io.MousePos.x))), 1, std::max(1, compiled_.frames));
    }

    // Pan with the middle button or Shift, zoom with the wheel about the
    // mouse -- Ctrl: the frames alone, Shift: the values alone.
    if (graphActive && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                        (ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.KeyShift))) {
        const float df = io.MouseDelta.x / gw * frameSpan, dv = io.MouseDelta.y / gh * valueSpan;
        curveFrom_ -= df;
        curveTo_ -= df;
        curveLo_ += dv;
        curveHi_ += dv;
    }
    if (overGraph && io.MouseWheel != 0.0f) {
        const float k = std::pow(0.85f, io.MouseWheel);
        if (!io.KeyShift) {
            const float at = frameAt(io.MousePos.x);
            curveFrom_ = at + (curveFrom_ - at) * k;
            curveTo_ = at + (curveTo_ - at) * k;
        }
        if (!io.KeyCtrl) {
            const float at = valueAt(io.MousePos.y);
            curveLo_ = at + (curveLo_ - at) * k;
            curveHi_ = at + (curveHi_ - at) * k;
        }
    }
    if (overGraph && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) frameCurves(curves);

    // The value lines across, and the frame lines down, as dense as there is
    // room for; the numbers at the left and along the ruler.
    d->PushClipRect(ImVec2(lo.x + names, lo.y), hi, true);
    const float vStep = tickStep(gh / valueSpan, theme::px(34.0f));
    for (float v = std::ceil(curveLo_ / vStep) * vStep; v <= curveHi_; v += vStep) {
        const float y = yOf(v);
        const bool zero = std::fabs(v) < 0.5f * vStep;
        d->AddLine(ImVec2(glo.x, y), ImVec2(ghi.x, y), zero ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 14));
        char n[32];
        std::snprintf(n, sizeof n, vStep < 0.1f ? "%.3g" : "%.4g", static_cast<double>(v));
        d->AddText(nullptr, ImGui::GetFontSize() * 0.78f, ImVec2(glo.x + theme::px(3.0f), y - ImGui::GetFontSize() * 0.5f),
                   theme::kTextFaint, n);
    }
    const float fStep = std::max(1.0f, tickStep(gw / frameSpan, theme::px(50.0f)));
    for (float f = std::ceil(curveFrom_ / fStep) * fStep; f <= curveTo_; f += fStep) {
        const float x = xOf(f);
        d->AddLine(ImVec2(x, glo.y), ImVec2(x, ghi.y), IM_COL32(255, 255, 255, 14));
        char n[32];
        std::snprintf(n, sizeof n, "%g", static_cast<double>(f));
        d->AddText(nullptr, ImGui::GetFontSize() * 0.78f, ImVec2(x + theme::px(3.0f), ghi.y + theme::px(2.0f)),
                   theme::kTextFaint, n);
    }
    // The shot: the frames outside it dimmed, so it is clear where it ends.
    const float shotEnd = xOf(static_cast<float>(std::max(1, compiled_.frames)));
    if (shotEnd < ghi.x) d->AddRectFilled(ImVec2(shotEnd, glo.y), ImVec2(ghi.x, ghi.y), IM_COL32(0, 0, 0, 60));
    const float first = xOf(1.0f);
    if (first > glo.x) d->AddRectFilled(ImVec2(glo.x, glo.y), ImVec2(first, ghi.y), IM_COL32(0, 0, 0, 60));
    // The play head.
    const float px = xOf(static_cast<float>(current_));
    d->AddLine(ImVec2(px, glo.y), ImVec2(px, ghi.y + ruler), theme::kAccent, theme::px(1.0f));

    // Each curve, sampled a pixel at a time -- through the evaluate() the
    // solver reads -- and its keys over it.
    auto picked = [&](const Curve& c, float frame) {
        return std::any_of(curvePicked_.begin(), curvePicked_.end(), [&](const CurveKey& k) {
            return k.node == c.node && k.component == c.component && k.param == c.def->name &&
                   std::fabs(k.frame - frame) <= kSame;
        });
    };
    const int columns = std::max(2, static_cast<int>(gw));
    std::vector<ImVec2> line;
    for (const Curve& c : curves) {
        const std::vector<sim::Key>* keys = net_.keys(c.node, c.def->name);
        if (!keys || keys->empty()) continue;
        line.clear();
        line.reserve(static_cast<size_t>(columns));
        for (int i = 0; i < columns; ++i) {
            const float frame = curveFrom_ + frameSpan * static_cast<float>(i) / static_cast<float>(columns - 1);
            const float v = sim::evaluate(*keys, frame, c.def->kind)[static_cast<size_t>(c.component)];
            line.push_back(ImVec2(glo.x + gw * static_cast<float>(i) / static_cast<float>(columns - 1), yOf(v)));
        }
        d->AddPolyline(line.data(), static_cast<int>(line.size()), c.color, 0, theme::px(1.6f));
        for (const sim::Key& k : *keys) {
            const ImVec2 at(xOf(k.frame), yOf(k.value[static_cast<size_t>(c.component)]));
            if (at.x < glo.x - 10.0f || at.x > ghi.x + 10.0f) continue;
            const bool on = picked(c, k.frame);
            const float r = theme::px(on ? 5.0f : 4.0f);
            const ImVec2 diamond[4] = {ImVec2(at.x, at.y - r), ImVec2(at.x + r, at.y), ImVec2(at.x, at.y + r),
                                       ImVec2(at.x - r, at.y)};
            d->AddConvexPolyFilled(diamond, 4, on ? theme::kAccent : c.color);
            d->AddPolyline(diamond, 4, IM_COL32(16, 17, 19, 255), ImDrawFlags_Closed, theme::px(1.4f));
        }
    }
    d->PopClipRect();

    // The names down the left, in each curve's colour.
    d->AddRectFilled(lo, ImVec2(lo.x + names, hi.y), theme::kPanel);
    d->AddLine(ImVec2(lo.x + names, lo.y), ImVec2(lo.x + names, hi.y), theme::kBorder);
    d->PushClipRect(lo, ImVec2(lo.x + names - theme::px(4.0f), hi.y), true);
    float ny = lo.y + theme::px(5.0f);
    const float step = ImGui::GetFontSize() + theme::px(3.0f);
    for (const Curve& c : curves) {
        if (ny + step > hi.y) {
            d->AddText(ImVec2(lo.x + theme::px(8.0f), ny), theme::kTextFaint, "\xe2\x80\xa6");
            break;
        }
        d->AddRectFilled(ImVec2(lo.x + theme::px(8.0f), ny + theme::px(4.0f)),
                         ImVec2(lo.x + theme::px(18.0f), ny + theme::px(6.0f)), c.color);
        d->AddText(nullptr, ImGui::GetFontSize() * 0.86f, ImVec2(lo.x + theme::px(23.0f), ny), theme::kTextDim,
                   c.label.c_str());
        ny += step;
    }
    d->PopClipRect();

    // --- picking and dragging the keys ----------------------------------------------

    // The key nearest the mouse, within reach.
    auto keyAt = [&](ImVec2 at, CurveKey& out) {
        float best = kReach * kReach * theme::px(1.0f) * theme::px(1.0f);
        bool found = false;
        for (const Curve& c : curves) {
            const std::vector<sim::Key>* keys = net_.keys(c.node, c.def->name);
            if (!keys) continue;
            for (const sim::Key& k : *keys) {
                const float dx = xOf(k.frame) - at.x, dy = yOf(k.value[static_cast<size_t>(c.component)]) - at.y;
                const float dist = dx * dx + dy * dy;
                if (dist > best) continue;
                best = dist;
                out = CurveKey{c.node, c.def->name, c.component, k.frame};
                found = true;
            }
        }
        return found;
    };
    auto contains = [&](const CurveKey& k) {
        return std::find(curvePicked_.begin(), curvePicked_.end(), k) != curvePicked_.end();
    };

    if (graphRightClicked) {
        CurveKey k;
        if (keyAt(io.MousePos, k)) {
            if (!contains(k)) curvePicked_ = {k};
            ImGui::OpenPopup("curves.key");
        }
    }
    if (ImGui::BeginPopup("curves.key")) {
        ImGui::TextDisabled("%zu key%s", curvePicked_.size(), curvePicked_.size() == 1 ? "" : "s");
        ImGui::Separator();
        for (const sim::Interp in : {sim::Interp::Smooth, sim::Interp::Linear, sim::Interp::Step}) {
            const char* names3[3] = {"Smooth to the next key", "Linear to the next key", "Step: hold to the next key"};
            if (!ImGui::MenuItem(names3[static_cast<int>(in)])) continue;
            for (const CurveKey& p : curvePicked_) {
                const std::vector<sim::Key>* keys = net_.keys(p.node, p.param);
                if (!keys) continue;
                for (const sim::Key& key : *keys) {
                    if (std::fabs(key.frame - p.frame) <= kSame) net_.setKey(p.node, p.param, p.frame, key.value, in);
                }
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) deleteCurveKeys();
        ImGui::EndPopup();
    }

    if (graphPressed && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyShift) {
        CurveKey k;
        if (keyAt(io.MousePos, k)) {
            if (io.KeyCtrl) {
                const auto at = std::find(curvePicked_.begin(), curvePicked_.end(), k);
                if (at != curvePicked_.end()) curvePicked_.erase(at);
                else curvePicked_.push_back(k);
            } else if (!contains(k)) {
                curvePicked_ = {k};
            }
            // What they were when the drag began: the drag works from there,
            // so that it does not creep as the keys move under it.
            curveDragged_.clear();
            for (const CurveKey& p : curvePicked_) {
                const std::vector<sim::Key>* keys = net_.keys(p.node, p.param);
                if (!keys) continue;
                for (const sim::Key& key : *keys) {
                    if (std::fabs(key.frame - p.frame) <= kSame) curveDragged_.push_back({p, key});
                }
            }
            curveDragFrom_ = io.MousePos;
            curveDragging_ = true;
        } else {
            if (!io.KeyCtrl) curvePicked_.clear();
            curveBoxFrom_ = io.MousePos;
            curveBoxing_ = true;
        }
    }

    if (curveDragging_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            curveDragging_ = false;
            curveDragged_.clear();
        } else if (io.MousePos.x != curveDragFrom_.x || io.MousePos.y != curveDragFrom_.y) {
            // Whole frames across, the component's value down; Ctrl holds
            // the frame, Shift the value.
            const float dFrame = io.KeyCtrl ? 0.0f
                                            : std::round((io.MousePos.x - curveDragFrom_.x) / gw * frameSpan);
            const float dValue = io.KeyShift ? 0.0f : -(io.MousePos.y - curveDragFrom_.y) / gh * valueSpan;
            // Where they are now, off first: one may land where another was,
            // and the drag works from where it began, not from here.
            for (const CurveKey& p : curvePicked_) net_.removeKey(p.node, p.param, p.frame);
            // A key once, however many of its components are dragged: it
            // holds them all, so each moves its own within the one key.
            struct Moved {
                int node;
                std::string param;
                float was;  ///< the frame it began at: what the snapshot is found by
                sim::Key key;
            };
            std::vector<Moved> moved;
            for (const auto& [p, key] : curveDragged_) {
                auto at = std::find_if(moved.begin(), moved.end(), [&](const Moved& m) {
                    return m.node == p.node && m.param == p.param && std::fabs(m.was - key.frame) <= kSame;
                });
                if (at == moved.end()) {
                    sim::Key put = key;
                    put.frame = std::max(1.0f, key.frame + dFrame);
                    moved.push_back(Moved{p.node, p.param, key.frame, put});
                    at = std::prev(moved.end());
                }
                const size_t c = static_cast<size_t>(p.component);
                at->key.value[c] = key.value[c] + dValue;
            }
            std::vector<CurveKey> now;
            for (const Moved& m : moved) net_.setKey(m.node, m.param, m.key.frame, m.key.value, m.key.interp);
            for (const auto& [p, key] : curveDragged_) {
                now.push_back(CurveKey{p.node, p.param, p.component, std::max(1.0f, key.frame + dFrame)});
            }
            curvePicked_ = std::move(now);
        }
    }
    if (curveBoxing_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            curveBoxing_ = false;
            // Every key inside the box, added to what was picked.
            const ImVec2 a(std::min(curveBoxFrom_.x, io.MousePos.x), std::min(curveBoxFrom_.y, io.MousePos.y));
            const ImVec2 b(std::max(curveBoxFrom_.x, io.MousePos.x), std::max(curveBoxFrom_.y, io.MousePos.y));
            for (const Curve& c : curves) {
                const std::vector<sim::Key>* keys = net_.keys(c.node, c.def->name);
                if (!keys) continue;
                for (const sim::Key& k : *keys) {
                    const float x = xOf(k.frame), y = yOf(k.value[static_cast<size_t>(c.component)]);
                    if (x < a.x || x > b.x || y < a.y || y > b.y) continue;
                    const CurveKey p{c.node, c.def->name, c.component, k.frame};
                    if (!contains(p)) curvePicked_.push_back(p);
                }
            }
        } else {
            d->AddRectFilled(curveBoxFrom_, io.MousePos, theme::fade(theme::kAccent, 0.12f));
            d->AddRect(curveBoxFrom_, io.MousePos, theme::kAccent);
        }
    }
    // A double click on the graph: a key on the nearest curve at that frame,
    // with the value it has there.
    if (overGraph && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const float frame = std::max(1.0f, std::round(frameAt(io.MousePos.x)));
        const Curve* nearest = nullptr;
        float best = 0.0f;
        for (const Curve& c : curves) {
            const std::vector<sim::Key>* keys = net_.keys(c.node, c.def->name);
            if (!keys || keys->empty()) continue;
            const float y = yOf(sim::evaluate(*keys, frame, c.def->kind)[static_cast<size_t>(c.component)]);
            const float dist = std::fabs(y - io.MousePos.y);
            if (nearest && dist >= best) continue;
            best = dist;
            nearest = &c;
        }
        if (nearest) {
            net_.setKey(nearest->node, nearest->def->name, frame, net_.valueAt(nearest->node, nearest->def->name, frame));
            curvePicked_ = {CurveKey{nearest->node, nearest->def->name, nearest->component, frame}};
            curveDragging_ = false;
        }
    }
    if (overGraph && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteCurveKeys();

    // What is under the mouse, and what is picked, at the top right.
    char note[160];
    CurveKey under;
    if (overGraph && keyAt(io.MousePos, under)) {
        std::snprintf(note, sizeof note, "%s  frame %g", under.param.c_str(), static_cast<double>(under.frame));
    } else if (!curvePicked_.empty()) {
        std::snprintf(note, sizeof note, "%zu key%s picked  \xc2\xb7  Del deletes", curvePicked_.size(),
                      curvePicked_.size() == 1 ? "" : "s");
    } else if (overGraph) {
        std::snprintf(note, sizeof note, "frame %.0f  \xc2\xb7  %.4g", static_cast<double>(frameAt(io.MousePos.x)),
                      static_cast<double>(valueAt(io.MousePos.y)));
    } else {
        note[0] = '\0';
    }
    if (note[0]) {
        const ImVec2 t = ImGui::CalcTextSize(note);
        ui::overlayText(d, ImVec2(ghi.x - t.x - theme::px(6.0f), glo.y + theme::px(4.0f)), theme::kTextDim, note);
    }
    ImGui::SetCursorScreenPos(ImVec2(lo.x, hi.y));
}

void SimWorkspace::deleteCurveKeys() {
    for (const CurveKey& k : curvePicked_) net_.removeKey(k.node, k.param, k.frame);
    curvePicked_.clear();
    curveDragged_.clear();
    curveDragging_ = false;
}

}  // namespace pg::editor
