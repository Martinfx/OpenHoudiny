// The editor's interface without a window: Dear ImGui runs on its own --
// frames built, input fed in, nothing drawn on a screen -- so what the panels,
// the menus and the node canvas do can be checked where there is no display.
#include "test_framework.h"

#include "NodeCanvas.h"
#include "Theme.h"
#include "Widgets.h"

#include "imgui.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace pg::editor::theme {
extern const unsigned char kInterRegular[];
extern const unsigned char kInterSemiBold[];
}  // namespace pg::editor::theme

namespace {

using namespace pg::editor;

/// A Dear ImGui context with the editor's theme, and no renderer: its
/// textures stay asked for, which is all a test needs.
struct Headless {
    explicit Headless(float width = 1280.0f, float height = 720.0f) {
        context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(width, height);
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        theme::apply(1.0f);
    }
    ~Headless() { ImGui::DestroyContext(context); }
    Headless(const Headless&) = delete;
    Headless& operator=(const Headless&) = delete;

    /// One frame: `draw` inside a window that fills the display.
    template <class F>
    void frame(const F& draw) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("test", nullptr, ImGuiWindowFlags_NoDecoration);
        draw();
        ImGui::End();
        ImGui::Render();
    }
    /// A key pressed in the next frame and let go in the one after.
    void press(ImGuiKey key) { pending.push_back(key); }
    void feed() {
        ImGuiIO& io = ImGui::GetIO();
        for (const ImGuiKey k : released) io.AddKeyEvent(k, false);
        released = pending;
        for (const ImGuiKey k : pending) io.AddKeyEvent(k, true);
        pending.clear();
    }

    ImGuiContext* context = nullptr;
    std::vector<ImGuiKey> pending, released;
};

bool popupOpen(const char* id) { return ImGui::IsPopupOpen(id); }

}  // namespace

TEST(editor_text_is_inter_with_czech_letters) {
    Headless ui;
    const theme::Fonts& f = theme::fonts();
    // Inter, the program's own copy, Regular and SemiBold.
    CHECK(f.regular && f.bold && f.mono);
    CHECK(f.regular != f.bold);
    CHECK(!f.regular->Sources.empty() && !f.bold->Sources.empty());
    CHECK(f.regular->Sources[0]->FontData == static_cast<const void*>(theme::kInterRegular));
    CHECK(f.bold->Sources[0]->FontData == static_cast<const void*>(theme::kInterSemiBold));
    ui.frame([&] {
        // The letters of Czech and the signs the editor writes, in Inter itself.
        for (const ImWchar c : {ImWchar(0x0159), ImWchar(0x016F), ImWchar(0x011B), ImWchar(0x017E), ImWchar(0x00D7),
                                ImWchar(0x00B7), ImWchar(0x2026), ImWchar(0x2014), ImWchar(0x25B6), ImWchar(0x2003)}) {
            CHECK(f.regular->IsGlyphInFont(c));
            CHECK(f.bold->IsGlyphInFont(c));
        }
    });
}

TEST(escape_closes_the_popup_on_top_and_not_a_dialog) {
    Headless ui;
    // A menu with a submenu open in it.
    ui.frame([&] {
        ImGui::OpenPopup("menu");
        if (ImGui::BeginPopup("menu")) {
            ImGui::OpenPopup("sub");
            if (ImGui::BeginPopup("sub")) ImGui::EndPopup();
            ImGui::EndPopup();
        }
    });
    auto both = [&](bool& menu, bool& sub, bool& closed) {
        ui.feed();
        ui.frame([&] {
            closed = ui::closePopupOnEscape();
            menu = sub = false;
            if (ImGui::BeginPopup("menu")) {
                menu = true;
                if (ImGui::BeginPopup("sub")) {
                    sub = true;
                    ImGui::EndPopup();
                }
                ImGui::EndPopup();
            }
        });
    };
    bool menu = false, sub = false, closed = false;
    both(menu, sub, closed);
    CHECK(menu && sub && !closed);
    // Escape: the submenu goes, the menu stays; again: the menu goes.
    ui.press(ImGuiKey_Escape);
    both(menu, sub, closed);
    CHECK(closed && menu && !sub);
    both(menu, sub, closed);  // let go
    ui.press(ImGuiKey_Escape);
    both(menu, sub, closed);
    CHECK(closed && !menu && !sub);
    both(menu, sub, closed);  // let go

    // A dialog keeps it for its own Cancel.
    ui.frame([&] {
        ImGui::OpenPopup("dialog");
        if (ImGui::BeginPopupModal("dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) ImGui::EndPopup();
    });
    ui.press(ImGuiKey_Escape);
    ui.feed();
    bool escapeSeen = false, open = false;
    ui.frame([&] {
        closed = ui::closePopupOnEscape();
        if (ImGui::BeginPopupModal("dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            escapeSeen = ImGui::IsKeyPressed(ImGuiKey_Escape);
            ImGui::EndPopup();
        }
        open = popupOpen("dialog");
    });
    CHECK(!closed);
    CHECK(escapeSeen);
    CHECK(open);
}

TEST(escape_that_closed_a_popup_reaches_nothing_under_it) {
    Headless ui;
    ui.frame([&] {
        ImGui::OpenPopup("menu");
        if (ImGui::BeginPopup("menu")) ImGui::EndPopup();
    });
    ui.press(ImGuiKey_Escape);
    ui.feed();
    bool under = true;
    ui.frame([&] {
        CHECK(ui::closePopupOnEscape());
        // A panel drawn after it -- the viewport, which cancels on Escape.
        under = ImGui::IsKeyPressed(ImGuiKey_Escape);
    });
    CHECK(!under);
}

TEST(pick_list_takes_the_lit_item_with_enter) {
    Headless ui;
    ui::PickList list;
    std::string search;
    const char* names[] = {"Box", "Sphere", "Tube", "Grid"};
    int chosen = -1, shown = 0;
    auto menu = [&] {
        ui.feed();
        ui.frame([&] {
            if (!popupOpen("add")) ImGui::OpenPopup("add");
            if (ImGui::BeginPopup("add")) {
                list.begin(search, 260.0f);
                list.heading("Geometry", IM_COL32(200, 80, 140, 255));
                shown = 0;
                for (int i = 0; i < 4; ++i) {
                    if (!search.empty() && std::string(names[i]).find(search) == std::string::npos) continue;
                    ++shown;
                    if (list.item(names[i], names[i], theme::Icon::Box, IM_COL32_WHITE, nullptr)) chosen = i;
                }
                list.end();
                ImGui::EndPopup();
            }
        });
    };
    menu();
    menu();
    CHECK_EQ(shown, 4);
    // Down twice, up once: the second; Enter takes it.
    for (const ImGuiKey k : {ImGuiKey_DownArrow, ImGuiKey_DownArrow, ImGuiKey_UpArrow}) {
        ui.press(k);
        menu();
        menu();
    }
    CHECK_EQ(chosen, -1);
    ui.press(ImGuiKey_Enter);
    menu();
    CHECK_EQ(chosen, 1);
    // Up from the first goes round to the last.
    chosen = -1;
    ui.press(ImGuiKey_UpArrow);
    menu();
    menu();
    ui.press(ImGuiKey_UpArrow);
    menu();
    menu();
    ui.press(ImGuiKey_Enter);
    menu();
    CHECK_EQ(chosen, 3);
}

TEST(pick_list_menu_is_no_taller_than_the_window) {
    // The list scrolls inside a menu of a fixed height: a long one opens
    // where it is asked for, all of it in the window.
    Headless ui(1280.0f, 720.0f);
    ui::PickList list;
    std::string search;
    ImVec2 lo, hi;
    ImGui::GetIO().AddMousePosEvent(900.0f, 600.0f);
    for (int frame = 0; frame < 4; ++frame) {
        ui.frame([&] {
            if (frame == 0) ImGui::OpenPopup("add");
            if (ImGui::BeginPopup("add")) {
                list.begin(search, 260.0f);
                for (int i = 0; i < 150; ++i) {
                    const std::string name = "Node " + std::to_string(i);
                    list.item(name.c_str(), name.c_str(), theme::Icon::Box, IM_COL32_WHITE, nullptr);
                }
                list.end();
                lo = ImGui::GetWindowPos();
                hi = ImVec2(lo.x + ImGui::GetWindowWidth(), lo.y + ImGui::GetWindowHeight());
                ImGui::EndPopup();
            }
        });
    }
    CHECK(lo.y >= 0.0f);
    CHECK(hi.y <= 720.0f);
    CHECK(hi.y - lo.y < 720.0f * 0.8f);
}

TEST(rows_put_every_value_in_one_column) {
    Headless ui;
    std::vector<float> x;
    float labels = 0.0f, widest = 0.0f;
    // A table measures its columns in its first frame: the second is it.
    for (int frame = 0; frame < 2; ++frame) ui.frame([&] {
        x.clear();
        labels = ImGui::GetCursorScreenPos().x;
        widest = ImGui::CalcTextSize("With gas").x;
        if (ui::beginRows("facts")) {
            for (const char* label : {"Gas", "Cells", "With gas", "Camera"}) {
                ui::row(label, "%s value", label);
                x.push_back(ImGui::GetItemRectMin().x);
            }
            ui::endRows();
        }
    });
    CHECK_EQ(x.size(), static_cast<size_t>(4));
    for (const float v : x) CHECK_NEAR(v, x[0], 0.5);
    // The values start after the widest label.
    CHECK(x[0] > labels + widest);
}

TEST(tab_header_switches_on_a_click) {
    Headless ui;
    int current = 0;
    ImVec2 second;
    auto draw = [&] {
        ui.frame([&] {
            const ui::PanelHeader h = ui::tabHeader(theme::Icon::Viewport, current, {"Viewport", "Render"}, "info");
            const float pad = theme::px(10.0f);
            const float first = theme::fonts().bold->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, "Viewport").x;
            second = ImVec2(h.min.x + theme::px(30.0f) - pad + first + 2.0f * pad + pad, (h.min.y + h.max.y) * 0.5f);
        });
    };
    draw();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(second.x + 4.0f, second.y);
    draw();
    io.AddMouseButtonEvent(0, true);
    draw();
    io.AddMouseButtonEvent(0, false);
    draw();
    CHECK_EQ(current, 1);
}

TEST(node_thumbnails_make_room_without_moving_the_network) {
    // Two nodes one over the other, as a network laid out without
    // thumbnails has them: the upper one's picture pushes the lower one down
    // as they are drawn -- where it stands in the network stays.
    Headless ui;
    NodeCanvas canvas;
    auto node = [](int id, float y) {
        CanvasNode n;
        n.id = id;
        n.title = "box" + std::to_string(id);
        n.y = y;
        n.outputs = {{"Geometry"}};
        return n;
    };
    std::vector<CanvasNode> nodes = {node(1, 0.0f), node(2, 60.0f)};
    std::map<int, ImVec2> moved;
    CanvasModel model;
    model.move = [&](int id, float x, float y) { moved[id] = ImVec2(x, y); };
    auto draw = [&] { ui.frame([&] { canvas.draw("net", nodes, {}, model); }); };
    draw();
    CHECK(canvas.view().lift.empty());  // without pictures: as they stand
    CHECK(canvas.thumbnailsShown().empty());

    for (CanvasNode& n : nodes) n.thumbnail = true;
    draw();
    const std::map<int, float> lift = canvas.view().lift;
    CHECK(!lift.count(1));  // the upper stays
    // The lower goes down by the picture -- 138 by 86 under a node 150 wide,
    // and a margin -- and the gap there was, a little at least.
    CHECK(lift.count(2) && std::fabs(lift.at(2) - 96.0f) < 0.5f);
    CHECK(moved.empty());  // the network is as it was
    CHECK_EQ(canvas.thumbnailsShown().size(), size_t(2));

    // A node new to the network stands where it was put: the others stay.
    nodes.push_back(node(3, 400.0f));
    nodes.back().thumbnail = true;
    draw();
    CHECK(canvas.view().lift == lift);

    // Laid out, every node goes where it is drawn: the room in the network itself.
    canvas.arrange();
    draw();
    CHECK(canvas.view().lift.empty());
    CHECK_EQ(moved.size(), size_t(3));
    for (const auto& [a, pa] : moved) {
        for (const auto& [b, pb] : moved) {
            // Stacked in one column, each below the other's picture.
            if (a < b && std::fabs(pa.x - pb.x) < 1.0f) CHECK(std::fabs(pa.y - pb.y) >= 150.0f);
        }
    }

    // Pictures off: drawn as they stand, no room kept.
    for (CanvasNode& n : nodes) n.thumbnail = false;
    draw();
    CHECK(canvas.view().lift.empty());
    CHECK(canvas.thumbnailsShown().empty());
}

TEST(node_names_cover_no_node_and_no_name) {
    // A network fitted far out: a grid of small nodes closer than their
    // names are wide -- as the demolition example's 78 nodes in a panel.
    std::vector<NameRoom> nodes;
    for (int col = 0; col < 6; ++col) {
        for (int row = 0; row < 20; ++row) {
            const ImVec2 lo(100.0f + 60.0f * static_cast<float>(col), 50.0f + 22.0f * static_cast<float>(row));
            const float name = 30.0f + static_cast<float>((col * 7 + row * 13) % 50);
            nodes.push_back({lo, ImVec2(lo.x + 40.0f, lo.y + 12.0f), ImVec2(name, 11.0f)});
        }
    }
    std::vector<size_t> order;
    for (size_t i = 0; i < nodes.size(); ++i) order.push_back((i * 37) % nodes.size());
    const auto at = placeNames(nodes, order, 3.0f);
    CHECK_EQ(at.size(), nodes.size());
    auto overlaps = [](ImVec2 a0, ImVec2 a1, ImVec2 b0, ImVec2 b1) {
        return a0.x < b1.x && b0.x < a1.x && a0.y < b1.y && b0.y < a1.y;
    };
    int placed = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!at[i].first) continue;
        ++placed;
        const ImVec2 lo = at[i].second, hi(lo.x + nodes[i].size.x, lo.y + nodes[i].size.y);
        for (size_t k = 0; k < nodes.size(); ++k) {
            if (k != i) CHECK(!overlaps(lo, hi, nodes[k].lo, nodes[k].hi));
            if (k != i && at[k].first) {
                const ImVec2 lo2 = at[k].second, hi2(lo2.x + nodes[k].size.x, lo2.y + nodes[k].size.y);
                CHECK(!overlaps(lo, hi, lo2, hi2));
            }
        }
    }
    // Some find room -- the right-hand column, the bottom row -- not all.
    CHECK(placed > 0);
    CHECK(placed < static_cast<int>(nodes.size()));
    // The first asked, with room under it, has its name there.
    const std::vector<NameRoom> two = {{ImVec2(0, 0), ImVec2(40, 12), ImVec2(30, 11)},
                                       {ImVec2(0, 14), ImVec2(40, 26), ImVec2(30, 11)}};
    const auto first = placeNames(two, {1, 0}, 3.0f);
    CHECK(first[1].first);
    CHECK_NEAR(first[1].second.y, 29.0, 0.5);
    // The other: not under itself (the second node is there), at its right.
    CHECK(first[0].first);
    CHECK(first[0].second.x >= 40.0f);
}
