//
// Headless shape-grammar demo: a block of buildings grown from a grid of lots,
// after "Generating 3d buildings using node graphs" (nelari.us, 2020).
//
// The same grammar is run twice -- once written as text, once wired up as a
// chain of rule nodes -- to show that the node graph is only another notation
// for it. Then one rule is edited, to show what non-destructive editing costs
// on a lazy cook engine: only the rules after the edit run again.
//
//   pgbuilding [out.obj] [--lots N]
//
// Writes out.obj plus out.mtl, one material per symbol. Open it in Blender or
// any other 3D package.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/grammar/Grammar.h"
#include "pg/nodes/Nodes.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>

using namespace pg;

namespace {

// Symbols are the words of the grammar, sizes are metres. The grammar runs in
// two stages with an ordinary pointwrangle in between: the first cuts streets
// and plots, the wrangle gives every plot its own height and roof pitch, the
// second grows the buildings. Shapes are plain geometry, so nothing special is
// needed to mix grammar rules with any other node.
constexpr const char* kPlotRules = R"(Lot         --> split(x) { 3: NIL | ~1: Strip | 3: NIL }
Strip       --> split(z) { 3: NIL | ~1: Parcel | 3: NIL }
Parcel      --> split(x) { ~1.3: Plot | 2: NIL | ~1: Plot }
)";

constexpr const char* kPlotAttributes =
    "@height = 4.6 + 3.2 * (1.0 + floor(noise(@P * 0.083) * 5.0));\n"
    "@pitch = 22.0 + floor(noise(@P * 0.117 + vec3(7.0, 0.0, 3.0)) * 3.0) * 6.0;\n";

constexpr const char* kBuildingRules = R"(Plot        --> extrude(height) Mass
Mass        --> comp(f) { front: Front | side: Side | top: RoofBase }
RoofBase    --> roofHip(pitch) Roof
Front       --> split(y) { 4: Shopfront | { ~3.2: Floor }* | 0.6: Cornice }
Side        --> split(y) { 4: Base | { ~3.2: Floor }* | 0.6: Cornice }
Cornice     --> extrude(0.35) Ledge
Shopfront   --> split(x) { 1: Pier | { ~4: ShopBay }* | 1: Pier }
ShopBay     --> split(x) { 0.4: Pier | ~1: ShopOpening | 0.4: Pier }
ShopOpening --> split(y) { 3: ShopWindow | ~1: Fascia }
ShopWindow  --> extrude(-0.3) ShopGlass
Floor       --> split(x) { 1: Wall | { ~3: Tile }* | 1: Wall }
Tile        --> split(x) { ~1: Wall | 1.3: WindowBay | ~1: Wall }
WindowBay   --> split(y) { 0.8: Wall | 0.12: Sill | 1.6: Window | ~1: Wall }
Sill        --> extrude(0.15) SillLedge
Window      --> extrude(-0.2) Glass
)";

/// Appends a rule node to the chain and sets its string parameters.
Node* rule(Graph& g, Node*& tail, const std::string& type, const std::string& name,
           std::initializer_list<std::pair<std::string, std::string>> params) {
    Node* n = g.create(type, name);
    n->setInput(0, tail);
    for (const auto& [key, value] : params) n->setString(key, value);
    tail = n;
    return n;
}

Node* split(Graph& g, Node*& tail, const std::string& name, const std::string& shape,
            const std::string& axis, const std::string& pattern) {
    return rule(g, tail, "split", name, {{"shape", shape}, {"axis", axis}, {"pattern", pattern}});
}

Node* extrude(Graph& g, Node*& tail, const std::string& name, const std::string& shape,
              float amount, const std::string& successor) {
    Node* n = rule(g, tail, "extrude", name, {{"shape", shape}, {"name", successor}});
    n->setFloat("amount", amount);
    return n;
}

/// The two stages of rules, one node per rule, each rule after the rules that
/// produce its symbol -- and the same pointwrangle in between.
Node* buildRuleChain(Graph& g, Node* tail) {
    split(g, tail, "streets_x", "Lot", "x", "{ 3: NIL | ~1: Strip | 3: NIL }");
    split(g, tail, "streets_z", "Strip", "z", "{ 3: NIL | ~1: Parcel | 3: NIL }");
    split(g, tail, "plots", "Parcel", "x", "{ ~1.3: Plot | 2: NIL | ~1: Plot }");
    rule(g, tail, "pointwrangle", "plot_attributes", {{"snippet", kPlotAttributes}});

    rule(g, tail, "extrude", "mass", {{"shape", "Plot"}, {"attrib", "height"}, {"name", "Mass"}});
    rule(g, tail, "comp", "faces", {{"shape", "Mass"}, {"front", "Front"}, {"side", "Side"},
                                    {"top", "RoofBase"}});
    rule(g, tail, "roof", "roof", {{"shape", "RoofBase"}, {"type", "hip"}, {"attrib", "pitch"},
                                   {"name", "Roof"}});
    split(g, tail, "front", "Front", "y", "{ 4: Shopfront | { ~3.2: Floor }* | 0.6: Cornice }");
    split(g, tail, "side", "Side", "y", "{ 4: Base | { ~3.2: Floor }* | 0.6: Cornice }");
    extrude(g, tail, "cornice", "Cornice", 0.35f, "Ledge");
    split(g, tail, "shopfront", "Shopfront", "x", "{ 1: Pier | { ~4: ShopBay }* | 1: Pier }");
    split(g, tail, "shopbay", "ShopBay", "x", "{ 0.4: Pier | ~1: ShopOpening | 0.4: Pier }");
    split(g, tail, "shopopening", "ShopOpening", "y", "{ 3: ShopWindow | ~1: Fascia }");
    extrude(g, tail, "shopwindow", "ShopWindow", -0.3f, "ShopGlass");
    split(g, tail, "floors", "Floor", "x", "{ 1: Wall | { ~3: Tile }* | 1: Wall }");
    split(g, tail, "tiles", "Tile", "x", "{ ~1: Wall | 1.3: WindowBay | ~1: Wall }");
    split(g, tail, "windowbay", "WindowBay", "y", "{ 0.8: Wall | 0.12: Sill | 1.6: Window | ~1: Wall }");
    extrude(g, tail, "sill", "Sill", 0.15f, "SillLedge");
    extrude(g, tail, "window", "Window", -0.2f, "Glass");
    return tail;
}

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

uint64_t totalCooks(const Graph& g) {
    uint64_t n = 0;
    for (Node* node : g.nodes()) n += node->cookCount();
    return n;
}

/// The derivation tree, read back from the `path` attribute: every symbol
/// with the number of finished shapes it ended up as.
void printDerivationTree(const Geometry& shapes) {
    struct TreeNode {
        size_t count = 0;
        std::map<std::string, TreeNode> children;
    };
    TreeNode root;
    const AttributeArray* path = shapes.points().find(grammar::kAttrPath);
    if (!path) return;
    const auto ids = path->read<int32_t>();
    for (int32_t id : ids) {
        const std::string& p = path->stringValue(id);
        TreeNode* node = &root;
        size_t begin = 0;
        while (begin <= p.size()) {
            size_t end = p.find('/', begin);
            if (end == std::string::npos) end = p.size();
            node = &node->children[p.substr(begin, end - begin)];
            ++node->count;
            begin = end + 1;
        }
    }

    std::printf("\nderivation tree (finished shapes under each symbol)\n");
    auto print = [](auto&& self, const TreeNode& n, int depth) -> void {
        for (const auto& [name, child] : n.children) {
            std::printf("  %*s%-*s %6zu\n", depth * 2, "", 28 - depth * 2, name.c_str(), child.count);
            self(self, child, depth + 1);
        }
    };
    print(print, root, 0);
}

Vec3 colourOf(const std::string& symbol) {
    static const std::map<std::string, Vec3> palette = {
        {"Wall", {0.86f, 0.78f, 0.66f}},      {"Pier", {0.76f, 0.67f, 0.55f}},
        {"Base", {0.55f, 0.53f, 0.50f}},      {"Fascia", {0.42f, 0.15f, 0.13f}},
        {"ShopGlass", {0.20f, 0.34f, 0.38f}}, {"Glass", {0.17f, 0.23f, 0.32f}},
        {"Ledge", {0.93f, 0.92f, 0.89f}},     {"SillLedge", {0.93f, 0.92f, 0.89f}},
        {"Roof", {0.62f, 0.27f, 0.18f}},
    };
    if (auto it = palette.find(symbol); it != palette.end()) return it->second;
    uint32_t h = 2166136261u;  // anything unknown still gets a stable colour
    for (char c : symbol) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return {0.3f + 0.6f * static_cast<float>(h & 0xff) / 255.0f,
            0.3f + 0.6f * static_cast<float>((h >> 8) & 0xff) / 255.0f,
            0.3f + 0.6f * static_cast<float>((h >> 16) & 0xff) / 255.0f};
}

/// OBJ with one material per symbol, taken from the `shape` primitive attribute.
bool writeObj(const Geometry& geo, const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const std::string mtlPath = (dot == std::string::npos ? path : path.substr(0, dot)) + ".mtl";
    const size_t slash = mtlPath.find_last_of("/\\");
    const std::string mtlName = slash == std::string::npos ? mtlPath : mtlPath.substr(slash + 1);

    std::ofstream obj(path);
    if (!obj) return false;
    obj << "# shape grammar demo, written by pgbuilding\n";
    obj << "mtllib " << mtlName << '\n';
    for (const Vec3& p : geo.positions()) obj << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';

    const AttributeArray* shape = geo.primitives().find(grammar::kAttrShape);
    std::set<std::string> used;
    std::string current;
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const std::string name = shape ? shape->stringValue(shape->read<int32_t>()[prim]) : "shape";
        if (name != current || prim == 0) {
            obj << "usemtl " << name << '\n';
            current = name;
            used.insert(name);
        }
        obj << 'f';
        for (uint32_t idx : geo.primitivePoints(prim)) obj << ' ' << (idx + 1);  // 1-based
        obj << '\n';
    }

    std::ofstream mtl(mtlPath);
    if (!mtl) return false;
    for (const auto& name : used) {
        const Vec3 c = colourOf(name);
        mtl << "newmtl " << name << "\nKd " << c.x << ' ' << c.y << ' ' << c.z << "\n\n";
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string objPath = "city.obj";
    int lotsPerSide = 4;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--lots") == 0 && i + 1 < argc) lotsPerSide = std::atoi(argv[++i]);
        else objPath = argv[i];
    }
    lotsPerSide = std::max(1, lotsPerSide);

    // --- build ----------------------------------------------------------------
    Graph graph;
    Node* grid = graph.create("grid", "grid");
    grid->setInt("rows", lotsPerSide + 1);
    grid->setInt("cols", lotsPerSide + 1);
    grid->setFloat("sizex", 30.0f * static_cast<float>(lotsPerSide));
    grid->setFloat("sizez", 30.0f * static_cast<float>(lotsPerSide));

    Node* lots = graph.create("lot", "lots");
    lots->setInput(0, grid);

    // The grammar as text: two shapegrammar nodes with the wrangle between them.
    Node* plots = graph.create("shapegrammar", "plot_grammar");
    plots->setInput(0, lots);
    plots->setString("rules", kPlotRules);
    Node* plotAttributes = graph.create("pointwrangle", "plot_attributes_text");
    plotAttributes->setInput(0, plots);
    plotAttributes->setString("snippet", kPlotAttributes);
    Node* text = graph.create("shapegrammar", "building_grammar");
    text->setInput(0, plotAttributes);
    text->setString("rules", kBuildingRules);

    // The same grammar as a chain of rule nodes.
    Node* chain = buildRuleChain(graph, lots);
    Node* out = graph.create("shapemesh", "mesh");
    out->setInput(0, chain);

    std::printf("shape grammar demo: %d x %d lots of 30 m\n", lotsPerSide, lotsPerSide);
    std::printf("\n1. shapegrammar: streets and plots\n%s", kPlotRules);
    std::printf("\n2. pointwrangle: a height and a roof pitch per plot\n%s", kPlotAttributes);
    std::printf("\n3. shapegrammar: buildings\n%s", kBuildingRules);

    // --- cook: the grammar as text, then as a node graph -----------------------
    CookEngine engine(1024ull * 1024 * 1024);
    const CookContext ctx;

    auto t0 = std::chrono::steady_clock::now();
    GeometryPtr fromText = engine.cook(*text, ctx);
    const double textMs = msSince(t0);
    for (Node* g : {plots, text}) {
        if (!grammarError(*g).empty()) {
            std::printf("%s: %s\n", g->name().c_str(), grammarError(*g).c_str());
            return 1;
        }
    }

    t0 = std::chrono::steady_clock::now();
    GeometryPtr fromNodes = engine.cook(*chain, ctx);
    const double nodesMs = msSince(t0);
    GeometryPtr meshed = engine.cook(*out, ctx);

    const bool same = fromText->hash() == fromNodes->hash();
    std::printf("\nderivation\n");
    std::printf("  text grammar   %zu shapes  %7.2f ms\n", fromText->pointCount(), textMs);
    std::printf("  rule nodes     %zu shapes  %7.2f ms\n", fromNodes->pointCount(), nodesMs);
    std::printf("  identical      %s  (hash %016llx)\n", same ? "yes" : "NO",
                static_cast<unsigned long long>(fromText->hash()));
    std::printf("  mesh           %zu points, %zu polygons\n", meshed->pointCount(),
                meshed->primitiveCount());

    printDerivationTree(*fromNodes);

    if (!writeObj(*meshed, objPath)) {
        std::printf("\ncould not write %s\n", objPath.c_str());
        return 1;
    }
    std::printf("\nwrote %s (+ .mtl, one material per symbol)\n", objPath.c_str());

    // --- non-destructive edit: wider windows ------------------------------------
    for (Node* n : graph.nodes()) n->resetCookCount();
    graph.find("tiles")->setString("pattern", "{ ~1: Wall | 1.9: WindowBay | ~1: Wall }");
    t0 = std::chrono::steady_clock::now();
    engine.cook(*out, ctx);
    std::printf("\nedit: wider windows in rule 'tiles'\n");
    std::printf("  recooked       %llu of the %zu nodes in the graph  %7.2f ms\n",
                static_cast<unsigned long long>(totalCooks(graph)), graph.size(), msSince(t0));
    return same ? 0 : 1;
}
