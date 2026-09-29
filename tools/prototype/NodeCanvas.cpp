#include "NodeCanvas.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <map>

namespace pg::editor {
namespace {

// Sizes in world units: pixels at zoom 1 on a display of the usual density.
constexpr float kFont = 15.0f;       // titles and pin labels
constexpr float kSmallFont = 13.0f;  // the subtitle, the summary
constexpr float kHeader = 28.0f;
constexpr float kRow = 22.0f;
constexpr float kPad = 10.0f;
constexpr float kPinRadius = 5.0f;
constexpr float kMinWidth = 150.0f;
constexpr float kRounding = 6.0f;

ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
ImVec2 operator*(ImVec2 a, float s) { return ImVec2(a.x * s, a.y * s); }

float textWidth(ImFont* font, float size, const std::string& s) {
    return s.empty() ? 0.0f : font->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str()).x;
}

/// Distance from p to the segment ab.
float segmentDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
    const ImVec2 ab = b - a, ap = p - a;
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    const float t = len2 > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
    const ImVec2 q = a + ab * t;
    return std::hypot(p.x - q.x, p.y - q.y);
}

float scaleOf(float zoom) { return zoom * theme::px(1.0f); }

ImVec2 bezier(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, float t) {
    const float u = 1.0f - t;
    const float w0 = u * u * u, w1 = 3.0f * u * u * t, w2 = 3.0f * u * t * t, w3 = t * t * t;
    return ImVec2(a.x * w0 + b.x * w1 + c.x * w2 + d.x * w3, a.y * w0 + b.y * w1 + c.y * w2 + d.y * w3);
}

}  // namespace

struct NodeCanvas::Layout {
    float width = kMinWidth;
    float height = kHeader;
    int rows = 0;
    bool summary = false;
};

NodeCanvas::Layout NodeCanvas::layoutOf(const CanvasNode& n) const {
    Layout l;
    const theme::Fonts& f = theme::fonts();
    float in = 0.0f, out = 0.0f;
    for (const CanvasPin& p : n.inputs) in = std::max(in, textWidth(f.regular, kFont, p.label));
    for (const CanvasPin& p : n.outputs) out = std::max(out, textWidth(f.regular, kFont, p.label));
    const float head = 34.0f + textWidth(f.bold, kFont, n.title) +
                       (n.subtitle.empty() ? 0.0f : 10.0f + textWidth(f.regular, kSmallFont, n.subtitle)) +
                       (n.problem ? 26.0f : 12.0f) + (n.displayable ? 20.0f : 0.0f);
    const float pins = in + out + 4.0f * kPad + (in > 0.0f && out > 0.0f ? 24.0f : 0.0f);
    const float summary = n.summary.empty() ? 0.0f : textWidth(f.regular, kSmallFont, n.summary) + 2.0f * kPad;
    l.width = std::ceil(std::max({kMinWidth, head, pins, summary}));
    l.rows = static_cast<int>(std::max(n.inputs.size(), n.outputs.size()));
    l.summary = !n.summary.empty();
    l.height = kHeader + static_cast<float>(l.rows) * kRow + (l.summary ? 18.0f : 0.0f) + (l.rows ? 8.0f : 6.0f);
    return l;
}

ImVec2 NodeCanvas::toScreen(ImVec2 world) const { return origin_ + pan_ + world * scaleOf(zoom_); }

ImVec2 NodeCanvas::toWorld(ImVec2 screen) const { return (screen - origin_ - pan_) * (1.0f / scaleOf(zoom_)); }

ImVec2 NodeCanvas::pinPosition(const CanvasNode& n, int pin, bool output) const {
    const float y = n.y + kHeader + 4.0f + (static_cast<float>(pin) + 0.5f) * kRow;
    return ImVec2(output ? n.x + layoutOf(n).width : n.x, y);
}

void NodeCanvas::select(int node, bool add) {
    if (!add) selection_.clear();
    selection_.insert(node);
    current_ = node;
}

void NodeCanvas::clearSelection() {
    selection_.clear();
    current_ = 0;
}

void NodeCanvas::toggle(int node) {
    if (selection_.erase(node) > 0) {
        if (current_ == node) current_ = selection_.empty() ? 0 : *selection_.begin();
        return;
    }
    selection_.insert(node);
    current_ = node;
}

void NodeCanvas::frame(bool selectionOnly) { frameRequest_ = selectionOnly ? 2 : 1; }

void NodeCanvas::reveal(int node) { revealNode_ = node; }

void NodeCanvas::arrange(bool selectionOnly) { arrangeRequest_ = selectionOnly ? 2 : 1; }

void NodeCanvas::layOut(const std::vector<CanvasNode>& nodes, const std::vector<CanvasLink>& links,
                        const CanvasModel& model, bool selectionOnly) {
    if (!model.move) return;
    std::vector<const CanvasNode*> chosen;
    for (const CanvasNode& n : nodes) {
        if (!selectionOnly || selection_.empty() || selection_.count(n.id)) chosen.push_back(&n);
    }
    if (chosen.empty()) return;
    std::map<int, size_t> index;
    for (size_t i = 0; i < chosen.size(); ++i) index[chosen[i]->id] = i;
    // Column: the longest way in from a node nothing feeds -- a DAG, so
    // relaxing the links as many times as there are nodes settles it.
    std::vector<int> column(chosen.size(), 0);
    for (size_t pass = 0; pass < chosen.size(); ++pass) {
        bool changed = false;
        for (const CanvasLink& k : links) {
            const auto a = index.find(k.from), b = index.find(k.to);
            if (a == index.end() || b == index.end()) continue;
            if (column[b->second] < column[a->second] + 1) {
                column[b->second] = column[a->second] + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    // A node that only feeds one column further on sits just before it.
    for (size_t i = 0; i < chosen.size(); ++i) {
        int next = INT32_MAX;
        for (const CanvasLink& k : links) {
            const auto a = index.find(k.from), b = index.find(k.to);
            if (a != index.end() && b != index.end() && a->second == i) next = std::min(next, column[b->second]);
        }
        if (next != INT32_MAX && next - 1 > column[i]) column[i] = next - 1;
    }
    const int columns = *std::max_element(column.begin(), column.end()) + 1;
    // Rows: the order they had, then by where what feeds them is.
    std::vector<std::vector<size_t>> byColumn(static_cast<size_t>(columns));
    for (size_t i = 0; i < chosen.size(); ++i) byColumn[static_cast<size_t>(column[i])].push_back(i);
    std::vector<float> row(chosen.size(), 0.0f);
    for (auto& c : byColumn) {
        std::sort(c.begin(), c.end(), [&](size_t a, size_t b) { return chosen[a]->y < chosen[b]->y; });
    }
    for (int sweep = 0; sweep < 4; ++sweep) {
        for (auto& c : byColumn) {
            for (size_t r = 0; r < c.size(); ++r) row[c[r]] = static_cast<float>(r);
        }
        for (size_t col = 1; col < byColumn.size(); ++col) {
            auto& c = byColumn[col];
            std::vector<std::pair<float, size_t>> keyed;
            for (size_t r = 0; r < c.size(); ++r) {
                float sum = 0.0f;
                int n = 0;
                for (const CanvasLink& k : links) {
                    const auto a = index.find(k.from), b = index.find(k.to);
                    if (a == index.end() || b == index.end() || b->second != c[r]) continue;
                    sum += row[a->second] + static_cast<float>(k.toPin) * 0.01f;
                    ++n;
                }
                keyed.push_back({n ? sum / static_cast<float>(n) : static_cast<float>(r), c[r]});
            }
            std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t r = 0; r < c.size(); ++r) c[r] = keyed[r].second;
        }
    }
    // Place: columns side by side, each centred on the same middle line,
    // the whole where the nodes were.
    float left = FLT_MAX, top = FLT_MAX;
    for (const CanvasNode* n : chosen) {
        left = std::min(left, n->x);
        top = std::min(top, n->y);
    }
    const float gapX = 70.0f, gapY = 26.0f;
    std::vector<float> heights(byColumn.size(), 0.0f);
    float tallest = 0.0f;
    for (size_t col = 0; col < byColumn.size(); ++col) {
        for (size_t i : byColumn[col]) heights[col] += layoutOf(*chosen[i]).height + gapY;
        heights[col] -= gapY;
        tallest = std::max(tallest, heights[col]);
    }
    float x = left;
    for (size_t col = 0; col < byColumn.size(); ++col) {
        float width = 0.0f;
        float y = top + (tallest - heights[col]) * 0.5f;
        for (size_t i : byColumn[col]) {
            const Layout l = layoutOf(*chosen[i]);
            model.move(chosen[i]->id, std::round(x), std::round(y));
            y += l.height + gapY;
            width = std::max(width, l.width);
        }
        x += width + gapX;
    }
}

bool NodeCanvas::flagRect(const CanvasNode& n, ImVec2& lo, ImVec2& hi) const {
    if (!n.displayable) return false;
    const float s = scaleOf(zoom_);
    const Layout l = layoutOf(n);
    const ImVec2 corner = toScreen(ImVec2(n.x + l.width, n.y));
    lo = ImVec2(corner.x - 17.0f * s, corner.y + 4.0f * s);
    hi = ImVec2(corner.x - 5.0f * s, corner.y + (kHeader - 4.0f) * s);
    return true;
}

void NodeCanvas::drawGrid(ImDrawList* d, ImVec2 lo, ImVec2 hi) const {
    const float s = scaleOf(zoom_);
    auto lines = [&](float step, ImU32 col) {
        const float px = step * s;
        if (px < 7.0f) return;
        const ImVec2 start = origin_ + pan_;  // where world (0, 0) is
        for (float x = start.x + std::ceil((lo.x - start.x) / px) * px; x < hi.x; x += px) {
            d->AddLine(ImVec2(x, lo.y), ImVec2(x, hi.y), col);
        }
        for (float y = start.y + std::ceil((lo.y - start.y) / px) * px; y < hi.y; y += px) {
            d->AddLine(ImVec2(lo.x, y), ImVec2(hi.x, y), col);
        }
    };
    lines(24.0f, IM_COL32(255, 255, 255, 7));
    lines(120.0f, IM_COL32(255, 255, 255, 14));
}

void NodeCanvas::drawLink(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 color, float thickness) const {
    const float bend = std::max(std::abs(b.x - a.x) * 0.5f, 40.0f * scaleOf(zoom_));
    d->AddBezierCubic(a, a + ImVec2(bend, 0.0f), b - ImVec2(bend, 0.0f), b, color, thickness);
}

void NodeCanvas::drawNode(ImDrawList* d, const CanvasNode& n, bool selected, bool hovered, const PinRef* hot,
                          bool hotAccepts) const {
    const float s = scaleOf(zoom_);
    const Layout l = layoutOf(n);
    const ImVec2 lo = toScreen(ImVec2(n.x, n.y));
    const ImVec2 hi = lo + ImVec2(l.width, l.height) * s;
    const float r = kRounding * s;
    const float a = n.dimmed ? 0.55f : 1.0f;
    const theme::Fonts& f = theme::fonts();

    // A soft shadow, then the body and the header.
    for (int i = 3; i >= 1; --i) {
        const float o = static_cast<float>(i) * 2.0f * s;
        d->AddRectFilled(lo + ImVec2(o * 0.5f, o), hi + ImVec2(o * 0.5f, o), IM_COL32(0, 0, 0, 22 * (4 - i)),
                         r + o);
    }
    ImU32 body = n.bypassed ? IM_COL32(58, 54, 36, 255) : IM_COL32(44, 46, 52, 255);
    d->AddRectFilled(lo, hi, theme::fade(body, a * 0.97f), r);
    ImU32 header = n.bypassed ? theme::shade(IM_COL32(150, 128, 40, 255), -0.2f) : n.color;
    if (hovered && !selected) header = theme::shade(header, 0.08f);
    d->AddRectFilled(lo, ImVec2(hi.x, lo.y + kHeader * s), theme::fade(header, a), r, ImDrawFlags_RoundCornersTop);
    // A line of light along the top of the header.
    d->AddLine(lo + ImVec2(r, 0.5f), ImVec2(hi.x - r, lo.y + 0.5f), theme::fade(IM_COL32(255, 255, 255, 40), a));

    // Zoomed far out, text too small to read is left out: the shapes and
    // the colours still show the network.
    const bool words = kFont * s >= 6.5f;
    const bool labels = kFont * s >= 8.5f;

    // Header: icon, title, subtitle, problem badge.
    const float cy = lo.y + kHeader * 0.5f * s;
    theme::drawIcon(d, n.icon, ImVec2(lo.x + 16.0f * s, cy), 15.0f * s, theme::fade(IM_COL32(255, 255, 255, 230), a));
    const float titleY = cy - kFont * 0.5f * s - 0.5f * s;
    if (words) {
        d->AddText(f.bold, kFont * s, ImVec2(lo.x + 30.0f * s, titleY), theme::fade(IM_COL32(255, 255, 255, 245), a),
                   n.title.c_str());
    }
    if (!words) {
        // Too far out for the text on it: its name under it, small but
        // readable -- as Houdini writes it beside a node -- so a whole
        // network fitted into the panel still says what is what.
        const float size = theme::px(11.0f);
        const float w = textWidth(f.bold, size, n.title);
        const ImVec2 at(std::floor((lo.x + hi.x - w) * 0.5f), std::floor(hi.y + 3.0f * theme::px(1.0f)));
        d->AddText(f.bold, size, at + ImVec2(1.0f, 1.0f), theme::fade(IM_COL32(0, 0, 0, 170), a), n.title.c_str());
        d->AddText(f.bold, size, at, theme::fade(IM_COL32(228, 230, 236, 255), a), n.title.c_str());
    }
    if (labels && !n.subtitle.empty()) {
        const float x = lo.x + (40.0f + textWidth(f.bold, kFont, n.title)) * s;
        d->AddText(f.regular, kSmallFont * s, ImVec2(x, cy - kSmallFont * 0.5f * s),
                   theme::fade(IM_COL32(255, 255, 255, 150), a), n.subtitle.c_str());
    }
    // The display flag at the right end: blue when on.
    const float flag = n.displayable ? 20.0f * s : 0.0f;
    ImVec2 flo, fhi;
    if (flagRect(n, flo, fhi)) {
        if (n.displayed) {
            d->AddRectFilled(flo, fhi, IM_COL32(58, 148, 255, 255), 3.0f * s);
            d->AddRect(flo, fhi, IM_COL32(180, 215, 255, 255), 3.0f * s, 0, 1.0f);
        } else {
            d->AddRect(flo, fhi, theme::fade(IM_COL32(255, 255, 255, hovered ? 120 : 55), a), 3.0f * s, 0, 1.2f * s);
        }
    }
    if (n.bypassed) {
        theme::drawIcon(d, theme::Icon::Bypass, ImVec2(hi.x - 14.0f * s - flag - (n.problem ? 20.0f * s : 0.0f), cy),
                        13.0f * s, IM_COL32(255, 220, 90, 255));
    }
    if (n.problem) {
        theme::drawIcon(d, n.problem == 2 ? theme::Icon::Error : theme::Icon::Warning,
                        ImVec2(hi.x - 14.0f * s - flag, cy), 15.0f * s, n.problem == 2 ? theme::kRed : theme::kYellow);
    }

    // Pins and their labels.
    auto pin = [&](const CanvasPin& p, int index, bool output) {
        const ImVec2 at = toScreen(pinPosition(n, index, output));
        const bool isHot = hot && hot->node == n.id && hot->pin == index && hot->output == output;
        ImU32 col = theme::fade(p.color, a);
        const float rad = kPinRadius * s * (isHot ? 1.35f : 1.0f);
        if (isHot) {
            d->AddCircleFilled(at, rad * 2.0f, theme::fade(hotAccepts ? theme::kGreen : theme::kRed, 0.25f));
        }
        if (p.many) {
            const ImVec2 h(rad * 0.85f, rad * 1.6f);
            if (p.links > 0) d->AddRectFilled(at - h, at + h, col, rad * 0.8f);
            else d->AddRectFilled(at - h, at + h, IM_COL32(30, 31, 35, 255), rad * 0.8f);
            d->AddRect(at - h, at + h, col, rad * 0.8f, 0, 1.6f * s);
        } else {
            if (p.links > 0) d->AddCircleFilled(at, rad, col);
            else d->AddCircleFilled(at, rad, IM_COL32(30, 31, 35, 255));
            d->AddCircle(at, rad, col, 0, 1.6f * s);
        }
        const float y = at.y - kFont * 0.5f * s - 0.5f * s;
        const ImU32 text = theme::fade(IM_COL32(210, 212, 218, 255), a);
        if (!labels) return;
        if (output) {
            const float w = textWidth(f.regular, kFont, p.label) * s;
            d->AddText(f.regular, kFont * s, ImVec2(at.x - (kPad + 4.0f) * s - w, y), text, p.label.c_str());
        } else {
            d->AddText(f.regular, kFont * s, ImVec2(at.x + (kPad + 4.0f) * s, y), text, p.label.c_str());
        }
    };
    for (size_t i = 0; i < n.inputs.size(); ++i) pin(n.inputs[i], static_cast<int>(i), false);
    for (size_t i = 0; i < n.outputs.size(); ++i) pin(n.outputs[i], static_cast<int>(i), true);
    if (l.summary && labels) {
        const float y = lo.y + (kHeader + static_cast<float>(l.rows) * kRow + 4.0f) * s;
        d->AddText(f.regular, kSmallFont * s, ImVec2(lo.x + kPad * s, y), theme::fade(theme::kTextDim, a),
                   n.summary.c_str());
    }

    // The outline: the selection in the accent colour.
    if (selected) {
        const bool current = n.id == current_;
        d->AddRect(lo - ImVec2(1.5f, 1.5f), hi + ImVec2(1.5f, 1.5f), theme::kAccent, r + 1.5f, 0,
                   current ? 2.5f : 1.8f);
        if (current) d->AddRect(lo - ImVec2(4, 4), hi + ImVec2(4, 4), theme::fade(theme::kAccent, 0.25f), r + 4.0f, 0, 3.0f);
    } else {
        d->AddRect(lo, hi, hovered ? IM_COL32(120, 124, 136, 255) : theme::fade(IM_COL32(70, 72, 80, 255), a), r, 0,
                   1.0f);
    }
}

void NodeCanvas::draw(const char* id, const std::vector<CanvasNode>& nodes, const std::vector<CanvasLink>& links,
                      const CanvasModel& model) {
    ImGui::PushID(id);
    ImGuiIO& io = ImGui::GetIO();
    origin_ = ImGui::GetCursorScreenPos();
    size_ = ImGui::GetContentRegionAvail();
    size_.x = std::max(size_.x, 50.0f);
    size_.y = std::max(size_.y, 50.0f);
    ImGui::InvisibleButton("canvas", size_,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput;
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 lo = origin_, hi = origin_ + size_;
    d->PushClipRect(lo, hi, true);
    d->AddRectFilled(lo, hi, theme::kCanvas);
    drawGrid(d, lo, hi);

    // Forget what is gone.
    auto find = [&](int nodeId) -> const CanvasNode* {
        for (const CanvasNode& n : nodes) {
            if (n.id == nodeId) return &n;
        }
        return nullptr;
    };
    for (auto it = selection_.begin(); it != selection_.end();) it = find(*it) ? std::next(it) : selection_.erase(it);
    if (current_ && !selection_.count(current_)) current_ = selection_.empty() ? 0 : *selection_.rbegin();

    // Framing.
    if (firstDraw_) frameRequest_ = 1;
    firstDraw_ = false;
    if (frameRequest_ && !nodes.empty()) {
        ImVec2 bmin(FLT_MAX, FLT_MAX), bmax(-FLT_MAX, -FLT_MAX);
        for (const CanvasNode& n : nodes) {
            if (frameRequest_ == 2 && !selection_.empty() && !selection_.count(n.id)) continue;
            const Layout l = layoutOf(n);
            bmin = ImVec2(std::min(bmin.x, n.x), std::min(bmin.y, n.y));
            bmax = ImVec2(std::max(bmax.x, n.x + l.width), std::max(bmax.y, n.y + l.height));
        }
        const float margin = 36.0f;
        const float fit = std::min(size_.x / (bmax.x - bmin.x + 2.0f * margin), size_.y / (bmax.y - bmin.y + 2.0f * margin)) /
                          theme::px(1.0f);
        zoom_ = std::clamp(fit, 0.15f, 1.1f);
        const ImVec2 middle = (bmin + bmax) * 0.5f;
        pan_ = size_ * 0.5f - middle * scaleOf(zoom_);
    }
    frameRequest_ = 0;
    // Laid out now, framed on the next draw: the nodes given to this one
    // are where they were.
    if (arrangeRequest_) {
        layOut(nodes, links, model, arrangeRequest_ == 2);
        arrangeRequest_ = 0;
        frameRequest_ = 1;
    }
    if (revealNode_) {
        if (const CanvasNode* n = find(revealNode_)) {
            const Layout l = layoutOf(*n);
            pan_ = size_ * 0.5f - ImVec2(n->x + l.width * 0.5f, n->y + l.height * 0.5f) * scaleOf(zoom_);
        }
        revealNode_ = 0;
    }

    const float s = scaleOf(zoom_);
    const ImVec2 mouse = io.MousePos;
    const bool inside = mouse.x >= lo.x && mouse.y >= lo.y && mouse.x < hi.x && mouse.y < hi.y;

    // What is under the mouse: a pin, else a node -- the topmost, drawn last.
    std::vector<const CanvasNode*> order;
    for (const CanvasNode& n : nodes) {
        if (!selection_.count(n.id)) order.push_back(&n);
    }
    for (const CanvasNode& n : nodes) {
        if (selection_.count(n.id)) order.push_back(&n);
    }
    int hoverNode = 0;
    PinRef hoverPin;
    bool onPin = false;
    if (inside && (hovered || drag_ != Drag::None)) {
        const float reach = std::max(10.0f, kPinRadius * 2.2f * s);
        float best = reach;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const CanvasNode& n = **it;
            for (int side = 0; side < 2; ++side) {
                const auto& pins = side ? n.outputs : n.inputs;
                for (size_t i = 0; i < pins.size(); ++i) {
                    const ImVec2 p = toScreen(pinPosition(n, static_cast<int>(i), side == 1));
                    const float dist = std::hypot(p.x - mouse.x, p.y - mouse.y);
                    if (dist < best) {
                        best = dist;
                        hoverPin = {n.id, static_cast<int>(i), side == 1};
                        onPin = true;
                    }
                }
            }
        }
        for (auto it = order.rbegin(); it != order.rend() && !hoverNode; ++it) {
            const Layout l = layoutOf(**it);
            const ImVec2 a = toScreen(ImVec2((*it)->x, (*it)->y)), b = a + ImVec2(l.width, l.height) * s;
            if (mouse.x >= a.x && mouse.y >= a.y && mouse.x <= b.x && mouse.y <= b.y) hoverNode = (*it)->id;
        }
    }
    // A link under the mouse, when nothing else is.
    int hoverLink = -1;
    auto linkEnds = [&](const CanvasLink& k, ImVec2& a, ImVec2& b) {
        const CanvasNode* from = find(k.from);
        const CanvasNode* to = find(k.to);
        if (!from || !to) return false;
        a = toScreen(pinPosition(*from, k.fromPin, true));
        b = toScreen(pinPosition(*to, k.toPin, false));
        return true;
    };
    if (inside && hovered && !onPin && !hoverNode && drag_ == Drag::None) {
        float best = 6.0f;
        for (size_t i = 0; i < links.size(); ++i) {
            ImVec2 a, b;
            if (!linkEnds(links[i], a, b)) continue;
            const float bend = std::max(std::abs(b.x - a.x) * 0.5f, 40.0f * s);
            ImVec2 prev = a;
            for (int k = 1; k <= 24; ++k) {
                const ImVec2 p = bezier(a, a + ImVec2(bend, 0.0f), b - ImVec2(bend, 0.0f), b, static_cast<float>(k) / 24.0f);
                const float dist = segmentDistance(mouse, prev, p);
                if (dist < best) {
                    best = dist;
                    hoverLink = static_cast<int>(i);
                }
                prev = p;
            }
        }
    }

    // --- the mouse ----------------------------------------------------------------------
    auto pinLinks = [&](const PinRef& p) {
        std::vector<CanvasLink> out;
        for (const CanvasLink& k : links) {
            if (p.output ? (k.from == p.node && k.fromPin == p.pin) : (k.to == p.node && k.toPin == p.pin)) {
                out.push_back(k);
            }
        }
        return out;
    };
    auto pinDef = [&](const PinRef& p) -> const CanvasPin* {
        const CanvasNode* n = find(p.node);
        if (!n) return nullptr;
        const auto& pins = p.output ? n->outputs : n->inputs;
        return p.pin >= 0 && static_cast<size_t>(p.pin) < pins.size() ? &pins[static_cast<size_t>(p.pin)] : nullptr;
    };
    const bool alt = io.KeyAlt, shift = io.KeyShift, ctrl = io.KeyCtrl;
    if (hovered && drag_ == Drag::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (alt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
            drag_ = Drag::Pan;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dragStart_ = mouse;
            dragMoved_ = false;
            if (ctrl && hoverLink >= 0) {
                if (model.disconnect) model.disconnect(links[static_cast<size_t>(hoverLink)]);
            } else if (onPin) {
                drag_ = Drag::Link;
                linkFrom_ = hoverPin;
                const CanvasPin* p = pinDef(hoverPin);
                // A linked input that takes one link: pick its link up, to
                // move it -- or, let go in space, to drop it.
                if (!hoverPin.output && p && !p->many && p->links > 0) {
                    const std::vector<CanvasLink> in = pinLinks(hoverPin);
                    if (!in.empty()) {
                        if (model.disconnect) model.disconnect(in.front());
                        linkFrom_ = {in.front().from, in.front().fromPin, true};
                    }
                }
            } else if (ImVec2 flo, fhi; hoverNode && find(hoverNode) && flagRect(*find(hoverNode), flo, fhi) &&
                       mouse.x >= flo.x - 2.0f && mouse.x <= fhi.x + 2.0f && mouse.y >= flo.y - 2.0f &&
                       mouse.y <= fhi.y + 2.0f) {
                if (model.toggleDisplay) model.toggleDisplay(hoverNode);
            } else if (hoverNode && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && model.open) {
                model.open(hoverNode);
            } else if (hoverNode) {
                drag_ = Drag::Nodes;
                pressedNode_ = hoverNode;
                if (shift) {
                    selection_.insert(hoverNode);
                } else if (ctrl) {
                    if (selection_.count(hoverNode)) selection_.erase(hoverNode);
                    else selection_.insert(hoverNode);
                } else if (!selection_.count(hoverNode)) {
                    selection_ = {hoverNode};
                }
                if (selection_.count(hoverNode)) current_ = hoverNode;
            } else {
                drag_ = Drag::Box;
                boxBase_ = shift || ctrl ? selection_ : std::set<int>{};
                if (!shift && !ctrl) clearSelection();
            }
        }
    }

    dropWhy_.clear();
    bool hotAccepts = false;
    const PinRef* hot = nullptr;
    PinRef target;
    if (drag_ == Drag::Link && onPin && hoverPin.output != linkFrom_.output && hoverPin.node != linkFrom_.node) {
        target = hoverPin;
        hot = &target;
        const PinRef& out = linkFrom_.output ? linkFrom_ : target;
        const PinRef& in = linkFrom_.output ? target : linkFrom_;
        hotAccepts = model.canConnect && model.canConnect(out, in, &dropWhy_);
    } else if (drag_ == Drag::None && onPin) {
        hot = &hoverPin;
        hotAccepts = true;
    }

    if (drag_ == Drag::Pan) {
        pan_ = pan_ + io.MouseDelta;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            drag_ = Drag::None;
        }
    } else if (drag_ == Drag::Nodes) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) dragMoved_ = true;
        if (dragMoved_ && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) && model.move) {
            const ImVec2 delta = io.MouseDelta * (1.0f / s);
            for (int nodeId : selection_) {
                if (const CanvasNode* n = find(nodeId)) model.move(nodeId, n->x + delta.x, n->y + delta.y);
            }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!dragMoved_ && !shift && !ctrl) {
                selection_ = {pressedNode_};
                current_ = pressedNode_;
            }
            // Where they came to rest, in whole units: files that diff well.
            if (dragMoved_ && model.move) {
                for (int nodeId : selection_) {
                    if (const CanvasNode* n = find(nodeId)) model.move(nodeId, std::round(n->x), std::round(n->y));
                }
            }
            drag_ = Drag::None;
        }
    } else if (drag_ == Drag::Box) {
        const ImVec2 a(std::min(dragStart_.x, mouse.x), std::min(dragStart_.y, mouse.y));
        const ImVec2 b(std::max(dragStart_.x, mouse.x), std::max(dragStart_.y, mouse.y));
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
            dragMoved_ = true;
            selection_ = boxBase_;
            for (const CanvasNode& n : nodes) {
                const Layout l = layoutOf(n);
                const ImVec2 p = toScreen(ImVec2(n.x, n.y)), q = p + ImVec2(l.width, l.height) * s;
                if (p.x < b.x && q.x > a.x && p.y < b.y && q.y > a.y) selection_.insert(n.id);
            }
            if (!selection_.count(current_)) current_ = selection_.empty() ? 0 : *selection_.begin();
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = Drag::None;
    } else if (drag_ == Drag::Link) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (hot && hotAccepts && model.connect) {
                model.connect(linkFrom_.output ? linkFrom_ : target, linkFrom_.output ? target : linkFrom_);
            } else if (!onPin && !hoverNode && inside && model.addMenu &&
                       std::hypot(mouse.x - dragStart_.x, mouse.y - dragStart_.y) > 12.0f) {
                // Dropped in space: add a node there, linked to the pin.
                menuAt_ = toWorld(mouse);
                menuPending_ = true;
                menuPin_ = linkFrom_;
                ImGui::OpenPopup("add");
            }
            drag_ = Drag::None;
        }
    }

    // Wheel: zoom round the mouse.
    if (hovered && io.MouseWheel != 0.0f && drag_ != Drag::Box) {
        const ImVec2 at = toWorld(mouse);
        zoom_ = std::clamp(zoom_ * std::pow(1.15f, io.MouseWheel), 0.15f, 2.5f);
        pan_ = mouse - origin_ - at * scaleOf(zoom_);
    }

    // Right click: the node's menu, or the add menu.
    const bool rightClick = ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                            io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16.0f;
    if (hovered && rightClick) {
        if (hoverNode) {
            if (!selection_.count(hoverNode)) selection_ = {hoverNode};
            current_ = hoverNode;
            menuNode_ = hoverNode;
            ImGui::OpenPopup("node");
        } else if (hoverLink >= 0) {
            menuNode_ = hoverLink;
            ImGui::OpenPopup("link");
        } else if (model.addMenu) {
            menuAt_ = toWorld(mouse);
            menuPending_ = false;
            ImGui::OpenPopup("add");
        }
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) pan_ = pan_ + io.MouseDelta;

    // --- the keyboard ---------------------------------------------------------------------
    // Keys go to the canvas under the mouse, as in Houdini -- or to the one
    // last clicked.
    const bool underMouse = inside && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !io.WantTextInput;
    if ((focused || underMouse) && drag_ == Drag::None) {
        std::vector<int> chosen(selection_.begin(), selection_.end());
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && model.addMenu) {
            menuAt_ = toWorld(inside ? mouse : origin_ + size_ * 0.5f);
            menuPending_ = false;
            ImGui::OpenPopup("add");
        }
        if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
             ImGui::IsKeyPressed(ImGuiKey_X, false)) &&
            !chosen.empty() && model.remove && !ctrl) {
            model.remove(chosen);
            clearSelection();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !ctrl) frame(!selection_.empty());
        if (ImGui::IsKeyPressed(ImGuiKey_L, false) && !ctrl) arrange(selection_.size() > 1);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
            for (const CanvasNode& n : nodes) selection_.insert(n.id);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D) && !chosen.empty() && model.duplicate) {
            model.duplicate(chosen);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_B, false) && !ctrl && !chosen.empty() && model.toggleBypass) {
            model.toggleBypass(chosen);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_I, false) && !ctrl && current_ && model.open) model.open(current_);
        if (ImGui::IsKeyPressed(ImGuiKey_U, false) && !ctrl && model.up) model.up();
        // Under the mouse only: over the viewport, R is its scale tool.
        if (underMouse && ImGui::IsKeyPressed(ImGuiKey_R, false) && !ctrl && current_ && model.toggleDisplay) {
            if (const CanvasNode* n = find(current_); n && n->displayable) model.toggleDisplay(current_);
        }
    }

    // --- drawing --------------------------------------------------------------------------
    const float thickness = std::max(1.5f, 2.6f * s);
    for (size_t i = 0; i < links.size(); ++i) {
        ImVec2 a, b;
        if (!linkEnds(links[i], a, b)) continue;
        const CanvasNode* to = find(links[i].to);
        const CanvasNode* from = find(links[i].from);
        const bool faint = (to && to->dimmed) || (from && (from->dimmed || from->bypassed));
        const bool lit = static_cast<int>(i) == hoverLink || selection_.count(links[i].from) || selection_.count(links[i].to);
        ImU32 col = faint ? theme::fade(links[i].color, 0.35f) : links[i].color;
        if (lit) col = theme::shade(col, 0.3f);
        drawLink(d, a, b, col, static_cast<int>(i) == hoverLink ? thickness * 1.6f : thickness);
    }
    for (const CanvasNode* n : order) {
        drawNode(d, *n, selection_.count(n->id) > 0, n->id == hoverNode && drag_ == Drag::None, hot, hotAccepts);
    }
    if (drag_ == Drag::Link) {
        if (const CanvasNode* n = find(linkFrom_.node)) {
            const CanvasPin* p = pinDef(linkFrom_);
            const ImVec2 a = toScreen(pinPosition(*n, linkFrom_.pin, linkFrom_.output));
            const ImVec2 b = hot ? toScreen(pinPosition(*find(target.node), target.pin, target.output)) : mouse;
            const ImU32 col = hot && !hotAccepts ? theme::kRed : (p ? p->color : theme::kText);
            if (linkFrom_.output) drawLink(d, a, b, col, thickness);
            else drawLink(d, b, a, col, thickness);
        }
    }
    if (drag_ == Drag::Box && dragMoved_) {
        const ImVec2 a(std::min(dragStart_.x, mouse.x), std::min(dragStart_.y, mouse.y));
        const ImVec2 b(std::max(dragStart_.x, mouse.x), std::max(dragStart_.y, mouse.y));
        d->AddRectFilled(a, b, theme::fade(theme::kAccent, 0.08f));
        d->AddRect(a, b, theme::fade(theme::kAccent, 0.8f));
    }
    if (nodes.empty()) {
        const char* hint = "Right click or Tab: add a node";
        const ImVec2 t = ImGui::CalcTextSize(hint);
        d->AddText(ImVec2(lo.x + (size_.x - t.x) * 0.5f, lo.y + (size_.y - t.y) * 0.5f), theme::kTextFaint, hint);
    }
    d->PopClipRect();

    // Why a link will not go where the mouse is.
    if (drag_ == Drag::Link && hot && !hotAccepts && !dropWhy_.empty()) ImGui::SetTooltip("%s", dropWhy_.c_str());
    else if (drag_ == Drag::None && hoverNode && hovered) {
        const CanvasNode* n = find(hoverNode);
        ImVec2 flo, fhi;
        const bool onFlag = n && flagRect(*n, flo, fhi) && mouse.x >= flo.x - 2.0f && mouse.x <= fhi.x + 2.0f &&
                            mouse.y >= flo.y - 2.0f && mouse.y <= fhi.y + 2.0f;
        if (onFlag) {
            ImGui::SetTooltip(n->displayed ? "Displayed: its geometry shows in the viewport. Click (or R) to hide it."
                                           : "Display flag: show this node's geometry in the viewport (R)");
        } else if (n && n->problem && !n->problemText.empty()) {
            const float flag = n->displayable ? 20.0f * s : 0.0f;
            const ImVec2 corner = toScreen(ImVec2(n->x + layoutOf(*n).width, n->y));
            if (mouse.x > corner.x - 28.0f * s - flag && mouse.y < corner.y + kHeader * s) {
                ImGui::SetTooltip("%s", n->problemText.c_str());
            }
        }
    }

    // --- popups ---------------------------------------------------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::px(8.0f), theme::px(8.0f)));
    if (ImGui::BeginPopup("add")) {
        if (model.addMenu && model.addMenu(menuAt_, menuPending_ ? &menuPin_ : nullptr)) {
            menuPending_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("node")) {
        if (model.nodeMenu) model.nodeMenu(menuNode_);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("link")) {
        if (menuNode_ >= 0 && static_cast<size_t>(menuNode_) < links.size() && ImGui::MenuItem("Remove link")) {
            if (model.disconnect) model.disconnect(links[static_cast<size_t>(menuNode_)]);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
}

}  // namespace pg::editor
