// The geometry of the Simulation network in the editor: the displayed
// node's geometry cooked for the viewport, what went wrong cooking, and the
// spreadsheet of a node's points, vertices, primitives, detail and volumes.
#include "SimWorkspace.h"

#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pg::editor {
namespace {

using theme::Icon;

const char* kClassNames[5] = {"Points", "Vertices", "Primitives", "Detail", "Volumes"};

std::string count(size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

/// One column of the spreadsheet: an attribute's component, or a group.
struct Column {
    std::string header;
    const AttributeArray* attribute = nullptr;
    int component = 0;            ///< of a vector
    const Group* group = nullptr;
};

std::string cell(const Column& c, size_t row) {
    char buf[64];
    if (c.group) return c.group->contains(row) ? "1" : "0";
    const AttributeArray& a = *c.attribute;
    if (row >= a.size()) return {};
    switch (a.type()) {
        case AttrType::Int: std::snprintf(buf, sizeof buf, "%d", a.read<int32_t>()[row]); break;
        case AttrType::Float: std::snprintf(buf, sizeof buf, "%.5g", static_cast<double>(a.read<float>()[row])); break;
        case AttrType::Vec2: {
            const Vec2 v = a.read<Vec2>()[row];
            std::snprintf(buf, sizeof buf, "%.5g", static_cast<double>(c.component == 0 ? v.x : v.y));
            break;
        }
        case AttrType::Vec3:
            std::snprintf(buf, sizeof buf, "%.5g", static_cast<double>(a.read<Vec3>()[row][c.component]));
            break;
        case AttrType::Vec4: {
            const Vec4 v = a.read<Vec4>()[row];
            const float x[4] = {v.x, v.y, v.z, v.w};
            std::snprintf(buf, sizeof buf, "%.5g", static_cast<double>(x[c.component]));
            break;
        }
        case AttrType::String: return a.stringValue(a.read<int32_t>()[row]);
    }
    return buf;
}

/// The columns of a class: P first, the rest by name, then its groups.
std::vector<Column> columnsOf(const Geometry& geo, AttrClass cls) {
    std::vector<Column> out;
    const AttributeSet& set = geo.attributes(cls);
    std::vector<std::string> names = set.names();
    std::stable_partition(names.begin(), names.end(), [](const std::string& n) { return n == "P"; });
    for (const std::string& name : names) {
        const AttributeArray* a = set.find(name);
        const int n = a->type() == AttrType::Vec2 ? 2 : a->type() == AttrType::Vec3 ? 3 : a->type() == AttrType::Vec4 ? 4 : 1;
        static const char* parts[4] = {"x", "y", "z", "w"};
        for (int k = 0; k < n; ++k) out.push_back({n == 1 ? name : name + "[" + parts[k] + "]", a, k, nullptr});
    }
    for (const std::string& name : geo.groupNames()) {
        const Group* g = geo.findGroup(name);
        if (g && g->classOf() == cls) out.push_back({"group:" + name, nullptr, 0, g});
    }
    return out;
}

}  // namespace

GeometryPtr SimWorkspace::geometryOf(int id) {
    // Now, on the window's thread: for an export, which waits for it.
    geometry_->sync(net_, folder());
    if (!geometry_->contains(id)) return nullptr;
    if (editingAsset()) feedAssetInputs();
    return geometry_->cook(id, shownFrame(), compiled_.world.timeStep);
}

int SimWorkspace::sheetNode() const {
    const int id = canvas_.current();
    return geometry_->contains(id) ? id : net_.displayed();
}

void SimWorkspace::updateGeometry() {
    // The window's graph knows which nodes are geometry nodes; the cooker cooks them.
    geometry_->sync(net_, folder());
    const int display = net_.displayed();
    const int sheet = sheetNode();
    // What is asked of the cooker -- again only when something in it changed:
    // the network, the levels gone into, the frame, the nodes wanted, the
    // simulation's frame (what Liquid Points reads).
    const std::shared_ptr<const sim::Frame> simFrame = runner_->frame(shownFrame());
    // With soft selection, what the shown Edit of what is picked moves:
    // the shares of a drag are of that.
    const int base = softBaseNode();
    char key[200];
    std::snprintf(key, sizeof key, "%llu/%llu/%d/%d/%d/%d/%p/%s", static_cast<unsigned long long>(net_.revision()),
                  static_cast<unsigned long long>(levelsRevision_), shownFrame(), display, sheet_ ? sheet : 0, base,
                  static_cast<const void*>(simFrame.get()), folder().c_str());
    if (key != cookKey_) {
        cookKey_ = key;
        sim::Cooker::Request r;
        for (const Level& l : levels_) r.levels.push_back({l.snapshot, l.folder, l.instance});
        r.levels.push_back({std::make_shared<const sim::Network>(net_), folder(), 0});
        r.frame = shownFrame();
        r.timeStep = compiled_.world.timeStep;
        if (display) r.nodes.push_back(display);
        if (sheet_ && sheet) r.nodes.push_back(sheet);
        if (base) r.nodes.push_back(base);
        cookSerial_ = cooker_->submit(std::move(r));
        cookAsked_ = ImGui::GetTime();
    }
    if (synchronous_) cooker_->wait();  // screenshots: the geometry of this frame
    sim::Cooker::Result done;
    if (!cooker_->take(done)) return;
    cookedSerial_ = done.serial;
    const auto shown = done.geometry.find(display);
    const GeometryPtr geo = shown != done.geometry.end() ? shown->second : nullptr;
    if (geo != renderer_.geometry()) {
        renderer_.setGeometry(geo);
        viewDirty_ = true;
    }
    const auto sheetGeo = done.geometry.find(sheet);
    sheetGeometry_ = sheetGeo != done.geometry.end() ? sheetGeo->second : nullptr;
    sheetGeometryNode_ = sheet;
    const auto baseGeo = base ? done.geometry.find(base) : done.geometry.end();
    softBase_ = baseGeo != done.geometry.end() ? baseGeo->second : nullptr;
    softBaseNode_ = softBase_ ? base : 0;
    // What went wrong the last time each node cooked.
    cookErrors_ = std::move(done.errors);
    cookWarnings_ = std::move(done.warnings);
    cookLogs_ = std::move(done.logs);
    cookMs_ = done.ms;
}

void SimWorkspace::spreadsheet() {
    // The current node's geometry, or else the displayed node's -- as the
    // cooker last made it.
    const int id = sheetNode();
    const sim::Node* n = net_.node(id);
    const GeometryPtr geo = n && sheetGeometryNode_ == id ? sheetGeometry_ : nullptr;
    if (!n || !geo) {
        ui::note("No geometry to show. Select a geometry node -- a Box, a Scatter, a Liquid Points... -- or "
                 "give one the display flag (the flag at its right end, or R).");
        return;
    }
    ImGui::PushFont(theme::fonts().bold, 0.0f);
    ImGui::TextUnformatted(n->name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    const std::string dot = " \xc2\xb7 ";
    ImGui::TextDisabled("%s", (count(geo->pointCount(), "point", "points") + dot + count(geo->vertexCount(), "vertex", "vertices") +
                               dot + count(geo->primitiveCount(), "primitive", "primitives") + dot +
                               count(geo->volumeCount(), "volume", "volumes"))
                                  .c_str());
    if (const auto e = cookErrors_.find(id); e != cookErrors_.end()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kRed));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(e->second.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    std::vector<const char*> classes(std::begin(kClassNames), std::end(kClassNames));
    ui::segmented("##class", sheetClass_, classes);

    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingFixedFit;
    ImGui::PushFont(theme::fonts().mono, 0.0f);
    const float cellWidth = ImGui::CalcTextSize("-0.00000e-00").x;
    if (sheetClass_ == 4) {
        // Volumes: one a row.
        if (ImGui::BeginTable("volumes", 8, flags)) {
            ImGui::TableSetupScrollFreeze(1, 1);
            for (const char* h : {"#", "name", "voxels", "voxel", "origin", "min", "max", "mean"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            const auto& volumes = geo->volumes();
            for (size_t i = 0; i < volumes.size(); ++i) {
                const Volume& v = volumes[i];
                float lo = 0.0f, hi = 0.0f;
                double sum = 0.0;
                if (v.values && !v.values->empty()) {
                    lo = hi = (*v.values)[0];
                    for (const float x : *v.values) {
                        lo = std::min(lo, x);
                        hi = std::max(hi, x);
                        sum += x;
                    }
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%zu", i);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(v.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d \xc3\x97 %d \xc3\x97 %d", v.res[0], v.res[1], v.res[2]);
                ImGui::TableNextColumn();
                ImGui::Text("%.4g", static_cast<double>(v.voxel));
                ImGui::TableNextColumn();
                ImGui::Text("%.3g %.3g %.3g", static_cast<double>(v.origin.x), static_cast<double>(v.origin.y),
                            static_cast<double>(v.origin.z));
                ImGui::TableNextColumn();
                ImGui::Text("%.4g", static_cast<double>(lo));
                ImGui::TableNextColumn();
                ImGui::Text("%.4g", static_cast<double>(hi));
                ImGui::TableNextColumn();
                ImGui::Text("%.4g", v.count() ? sum / static_cast<double>(v.count()) : 0.0);
            }
            ImGui::EndTable();
        }
        ImGui::PopFont();
        return;
    }
    const AttrClass cls = sheetClass_ == 0 ? AttrClass::Point
                          : sheetClass_ == 1 ? AttrClass::Vertex
                          : sheetClass_ == 2 ? AttrClass::Primitive
                                             : AttrClass::Detail;
    const std::vector<Column> columns = columnsOf(*geo, cls);
    const size_t rows = geo->elementCount(cls);
    // The topology first: a vertex's point; a primitive's corners.
    const int topology = cls == AttrClass::Vertex ? 1 : cls == AttrClass::Primitive ? 2 : 0;
    const int total = 1 + topology + static_cast<int>(columns.size());
    if (total > 500) {
        ImGui::TextDisabled("Too many columns to show: %d", total);
    } else if (ImGui::BeginTable("sheet", total, flags)) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000000").x);
        if (cls == AttrClass::Vertex) ImGui::TableSetupColumn("point", ImGuiTableColumnFlags_WidthFixed, cellWidth);
        if (cls == AttrClass::Primitive) {
            ImGui::TableSetupColumn("closed", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("closed").x);
            ImGui::TableSetupColumn("points", ImGuiTableColumnFlags_WidthFixed, cellWidth * 2.0f);
        }
        for (const Column& c : columns) {
            ImGui::TableSetupColumn(c.header.c_str(), ImGuiTableColumnFlags_WidthFixed,
                                    std::max(cellWidth, ImGui::CalcTextSize(c.header.c_str()).x));
        }
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(std::min<size_t>(rows, 100000000)));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const size_t row = static_cast<size_t>(r);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%zu", row);
                if (cls == AttrClass::Vertex) {
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", geo->vertexPoint(row));
                }
                if (cls == AttrClass::Primitive) {
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(geo->primitiveClosed(row) ? "yes" : "no");
                    ImGui::TableNextColumn();
                    std::string corners;
                    const auto pts = geo->primitivePoints(row);
                    for (size_t k = 0; k < pts.size() && k < 8; ++k) corners += (k ? " " : "") + std::to_string(pts[k]);
                    if (pts.size() > 8) corners += " \xe2\x80\xa6";
                    ImGui::TextUnformatted(corners.c_str());
                }
                for (const Column& c : columns) {
                    ImGui::TableNextColumn();
                    const std::string text = cell(c, row);
                    ImGui::TextUnformatted(text.c_str());
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
}

}  // namespace pg::editor
