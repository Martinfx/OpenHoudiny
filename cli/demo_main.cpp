//
// Headless demo: builds a small procedural graph, cooks it, prints a
// spreadsheet-style dump and writes an OBJ you can open in any 3D package.
//
// Headless is the point. Everything the (future) GUI can do has to be
// reachable from a library call first -- render farms have no viewport.
//
//   pgdemo [out.obj] [--frames N]
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace pg;

namespace {

void printSpreadsheet(const Geometry& geo, size_t maxRows) {
    std::printf("\ngeometry spreadsheet (points)\n");
    const auto names = geo.points().names();

    std::printf("  %6s", "ptnum");
    for (const auto& n : names) {
        const AttributeArray* a = geo.points().find(n);
        std::printf("  %-26s", (n + " (" + attrTypeName(a->type()) + ")").c_str());
    }
    std::printf("\n");

    const size_t rows = std::min(maxRows, geo.pointCount());
    for (size_t i = 0; i < rows; ++i) {
        std::printf("  %6zu", i);
        for (const auto& n : names) {
            const AttributeArray* a = geo.points().find(n);
            char cell[64];
            switch (a->type()) {
                case AttrType::Vec3: {
                    const Vec3& v = a->read<Vec3>()[i];
                    std::snprintf(cell, sizeof(cell), "%.3f, %.3f, %.3f", v.x, v.y, v.z);
                    break;
                }
                case AttrType::Float:
                    std::snprintf(cell, sizeof(cell), "%.4f", a->read<float>()[i]);
                    break;
                case AttrType::Int:
                    std::snprintf(cell, sizeof(cell), "%d", a->read<int32_t>()[i]);
                    break;
                case AttrType::String:
                    std::snprintf(cell, sizeof(cell), "\"%s\"",
                                  a->stringValue(a->read<int32_t>()[i]).c_str());
                    break;
                default:
                    std::snprintf(cell, sizeof(cell), "-");
                    break;
            }
            std::printf("  %-26s", cell);
        }
        std::printf("\n");
    }
    if (geo.pointCount() > rows) {
        std::printf("  ... %zu more points\n", geo.pointCount() - rows);
    }
}

bool writeObj(const Geometry& geo, const std::string& path) {
    std::ofstream out(path);
    if (!out) return false;

    out << "# written by the procedural geometry core prototype\n";
    auto P = geo.positions();
    for (const Vec3& p : P) out << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';

    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        auto pts = geo.primitivePoints(prim);
        if (pts.size() < 2) continue;
        out << (geo.primitiveClosed(prim) ? "f" : "l");
        for (uint32_t idx : pts) out << ' ' << (idx + 1);  // OBJ is 1-based
        out << '\n';
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string objPath = "out.obj";
    int frames = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::atoi(argv[++i]);
        else objPath = argv[i];
    }

    // --- build ------------------------------------------------------------
    Graph graph;
    Node* grid = graph.create("grid", "grid");
    grid->setInt("rows", 200);
    grid->setInt("cols", 200);
    grid->setFloat("sizex", 10.0f);
    grid->setFloat("sizez", 10.0f);

    Node* displace = graph.create("pointwrangle", "displace");
    displace->setInput(0, grid);
    displace->setString("snippet",
        "@P.y = noise(@P * 0.45 + vec3(@Time, 0.0, 0.0)) * 2.0 - 1.0;"
        "@height = @P.y;"
        "@Cd = vec3(fit(@P.y, -1.0, 1.0, 0.1, 1.0), 0.4, 0.8);");

    Node* selection = graph.create("groupbox", "selection");
    selection->setInput(0, displace);
    selection->setString("name", "middle");
    selection->setVec3("min", Vec3(-1.5f, -10.0f, -1.5f));
    selection->setVec3("max", Vec3(1.5f, 10.0f, 1.5f));

    Node* hole = graph.create("blast", "hole");
    hole->setInput(0, selection);
    hole->setString("group", "middle");

    Node* out = graph.create("transform", "out");
    out->setInput(0, hole);
    out->setVec3("r", Vec3(0, 15, 0));

    // --- cook -------------------------------------------------------------
    CookEngine engine(512ull * 1024 * 1024);
    std::printf("graph: %zu nodes, time dependent: %s\n", graph.size(),
                engine.isTimeDependent(*out) ? "yes" : "no");

    GeometryPtr result;
    for (int f = 1; f <= frames; ++f) {
        result = engine.cook(*out, CookContext{f / 24.0, f, 24.0});
    }

    std::printf("\nresult\n");
    std::printf("  points       %zu\n", result->pointCount());
    std::printf("  primitives   %zu\n", result->primitiveCount());
    std::printf("  memory       %.2f MB\n",
                static_cast<double>(result->memoryUsage()) / (1024 * 1024));
    std::printf("  hash         %016llx\n", static_cast<unsigned long long>(result->hash()));

    std::printf("\ncook\n");
    std::printf("  frames cooked      %d\n", frames);
    std::printf("  grid cooks         %llu   (time independent, cooked once)\n",
                static_cast<unsigned long long>(grid->cookCount()));
    std::printf("  displace cooks     %llu   (reads @Time, so once per frame)\n",
                static_cast<unsigned long long>(displace->cookCount()));
    std::printf("  cache hits/misses  %llu / %llu\n",
                static_cast<unsigned long long>(engine.cache().hits()),
                static_cast<unsigned long long>(engine.cache().misses()));

    printSpreadsheet(*result, 8);

    if (writeObj(*result, objPath)) {
        std::printf("\nwrote %s\n", objPath.c_str());
    } else {
        std::printf("\ncould not write %s\n", objPath.c_str());
        return 1;
    }
    return 0;
}
