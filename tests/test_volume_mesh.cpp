//
// Volumes to polygons (volumeToMesh, the Convert Volume node): the surface
// of a ball is a closed mesh -- each edge between two faces, turned
// alike --, round and as big as the ball, its normals outward; what fills
// the grid is closed where the grid ends; the same bits on any number of
// threads, and from a volume kept in tiles as from every voxel.
//
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

using namespace pg;

namespace {

/// A ball of radius r round c: its distance (below 0 inside), or a density
/// of 1 inside fading to 0 over a voxel.
Volume ball(float r, const Vec3& c, bool distance, int n = 24, float voxel = 0.05f) {
    const Vec3 origin = c - Vec3(0.5f * voxel * static_cast<float>(n), 0.5f * voxel * static_cast<float>(n),
                                 0.5f * voxel * static_cast<float>(n));
    std::vector<float> values;
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const Vec3 p = origin + Vec3((static_cast<float>(i) + 0.5f) * voxel, (static_cast<float>(j) + 0.5f) * voxel,
                                             (static_cast<float>(k) + 0.5f) * voxel);
                const float d = length(p - c) - r;
                values.push_back(distance ? d : std::clamp(0.5f - d / voxel, 0.0f, 1.0f));
            }
        }
    }
    return Volume::make(distance ? "surface" : "density", origin, voxel, n, n, n, std::move(values));
}

/// Each edge of the faces: how many faces go along it each way.
struct Edges {
    std::map<std::pair<uint32_t, uint32_t>, int> along;
    explicit Edges(const Geometry& g) {
        for (size_t f = 0; f < g.primitiveCount(); ++f) {
            const auto c = g.primitivePoints(f);
            for (size_t i = 0; i < c.size(); ++i) ++along[{c[i], c[(i + 1) % c.size()]}];
        }
    }
    /// Closed and turned alike: each edge once each way.
    bool closed() const {
        for (const auto& [e, n] : along) {
            const auto back = along.find({e.second, e.first});
            if (n != 1 || back == along.end() || back->second != 1) return false;
        }
        return !along.empty();
    }
};

/// What the faces hold (the divergence theorem over their fans).
double enclosed(const Geometry& g) {
    const auto P = g.positions();
    double v = 0.0;
    for (size_t f = 0; f < g.primitiveCount(); ++f) {
        const auto c = g.primitivePoints(f);
        for (size_t i = 1; i + 1 < c.size(); ++i) {
            const Vec3 &a = P[c[0]], &b = P[c[i]], &d = P[c[i + 1]];
            v += static_cast<double>(dot(a, cross(b, d))) / 6.0;
        }
    }
    return v;
}

}  // namespace

TEST(volume_mesh_of_a_ball_is_closed_round_and_turned_outward) {
    const Vec3 c(0.1f, 0.6f, -0.2f);
    const float r = 0.37f;
    for (const bool distance : {true, false}) {
        const Volume v = ball(r, c, distance);
        const auto mesh = volumeToMesh(v, distance ? 0.0f : 0.5f, distance);
        CHECK(mesh->primitiveCount() > 500);
        CHECK(Edges(*mesh).closed());
        const auto P = mesh->positions();
        const AttributeArray* n = mesh->points().find("N");
        CHECK(n != nullptr);
        if (!n) continue;
        const auto N = n->read<Vec3>();
        float worst = 0.0f, inward = 1.0f;
        for (size_t i = 0; i < P.size(); ++i) {
            worst = std::max(worst, std::fabs(length(P[i] - c) - r));
            inward = std::min(inward, dot(N[i], normalize(P[i] - c)));
        }
        CHECK(worst < 0.25f * v.voxel);  // on the ball, within a quarter of a voxel
        CHECK(inward > 0.9f);            // every normal outward
        const double ideal = 4.0 / 3.0 * 3.14159265358979 * r * r * r;
        CHECK_NEAR(enclosed(*mesh) / ideal, 1.0, 0.03);
    }
}

TEST(volume_mesh_closes_what_fills_the_grid_where_the_grid_ends) {
    // All inside: a box as big as the grid, closed, turned outward -- its
    // faces on the grid's, its edges and corners cut off a little (surface
    // nets round what is sharp: an edge's point is the mean of where the
    // surface crosses its cube).
    const Volume full = Volume::make("density", Vec3(-1.0f, 0.0f, 2.0f), 0.25f, 4, 3, 2, std::vector<float>(24, 1.0f));
    const auto box = volumeToMesh(full, 0.5f, false);
    CHECK(Edges(*box).closed());
    CHECK(enclosed(*box) < 1.0 * 0.75 * 0.5);
    CHECK(enclosed(*box) > 0.65 * 1.0 * 0.75 * 0.5);
    Vec3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
    for (const Vec3& p : box->positions()) {
        lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    CHECK_NEAR(lo.x, -1.0f, 1e-6f);
    CHECK_NEAR(lo.y, 0.0f, 1e-6f);
    CHECK_NEAR(lo.z, 2.0f, 1e-6f);
    CHECK_NEAR(hi.x, 0.0f, 1e-6f);
    CHECK_NEAR(hi.y, 0.75f, 1e-6f);
    CHECK_NEAR(hi.z, 2.5f, 1e-6f);
    // One voxel inside: a closed little cube round it.
    std::vector<float> one(27, 0.0f);
    one[13] = 1.0f;
    const auto dot = volumeToMesh(Volume::make("d", Vec3(), 1.0f, 3, 3, 3, one), 0.5f, false);
    CHECK_EQ(dot->pointCount(), size_t(8));
    CHECK_EQ(dot->primitiveCount(), size_t(6));
    CHECK(Edges(*dot).closed());
    CHECK(enclosed(*dot) > 0.0);
    // Nothing inside, or nothing at all: no mesh.
    CHECK_EQ(volumeToMesh(Volume::make("d", Vec3(), 1.0f, 3, 3, 3, std::vector<float>(27, 0.0f)), 0.5f, false)->pointCount(),
             size_t(0));
    CHECK_EQ(volumeToMesh(Volume(), 0.5f, false)->pointCount(), size_t(0));
}

TEST(volume_mesh_is_the_same_on_any_number_of_threads_and_as_the_node_gives_it) {
    const Volume v = ball(0.3f, Vec3(0.0f, 0.5f, 0.0f), false, 40, 0.02f);
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    const uint64_t one = volumeToMesh(v, 0.5f, false)->hash();
    TaskPool::instance().setThreadCount(4);
    const uint64_t four = volumeToMesh(v, 0.5f, false)->hash();
    TaskPool::instance().setThreadCount(saved);
    CHECK_EQ(one, four);

    // The node: the volume named -- else the first -- where it crosses Iso.
    registerBuiltinNodes();
    auto node = NodeRegistry::instance().create("convertvolume", "convert");
    CHECK(node != nullptr);
    if (!node) return;
    auto geo = std::make_shared<Geometry>();
    geo->addVolume(Volume::make("other", Vec3(), 1.0f, 2, 2, 2, std::vector<float>(8, 1.0f)));
    geo->addVolume(v);
    const GeometryPtr in[1] = {geo};
    node->setString("volume", "density");
    node->setFloat("iso", 0.5f);
    CHECK_EQ(node->cookNode(CookContext{}, in)->hash(), one);
    node->setString("volume", "");
    const auto first = node->cookNode(CookContext{}, in);
    CHECK_EQ(first->primitiveCount(), size_t(6 * 4));  // the 2 x 2 x 2 box of "other"
    CHECK_EQ(first->volumeCount(), size_t(0));
    node->setFloat("iso", 2.0f);
    CHECK_EQ(node->cookNode(CookContext{}, in)->pointCount(), size_t(0));  // nothing above 2
    node->setInt("inside", 1);
    CHECK_EQ(node->cookNode(CookContext{}, in)->primitiveCount(), size_t(6 * 4));  // all below 2
}

TEST(volume_mesh_of_tiles_is_that_of_every_voxel) {
    // Two balls' distances, cut off at a band, in a grid whose sides are no
    // whole number of tiles -- the second ball through the grid's +x side;
    // kept, the tiles with anything nearer than the band.
    const int n[3] = {37, 29, 45};
    const float voxel = 0.05f, band = 0.12f, r = 0.4f;
    const Vec3 origin(-0.4f, 0.1f, -1.0f);
    const Vec3 centres[2] = {origin + Vec3(0.6f, 0.7f, 1.1f), origin + Vec3(1.7f, 0.6f, 1.3f)};
    auto distance = [&](int i, int j, int k) {
        const Vec3 p = origin + Vec3((static_cast<float>(i) + 0.5f) * voxel, (static_cast<float>(j) + 0.5f) * voxel,
                                     (static_cast<float>(k) + 0.5f) * voxel);
        return std::min(std::min(length(p - centres[0]), length(p - centres[1])) - r, band);
    };
    std::vector<float> every;
    for (int k = 0; k < n[2]; ++k) {
        for (int j = 0; j < n[1]; ++j) {
            for (int i = 0; i < n[0]; ++i) every.push_back(distance(i, j, k));
        }
    }
    const Volume whole = Volume::make("surface", origin, voxel, n[0], n[1], n[2], every);
    TiledVolume tiled;
    tiled.origin = origin;
    tiled.voxel = voxel;
    for (int a = 0; a < 3; ++a) tiled.res[a] = n[a];
    tiled.background = band;
    const int t[3] = {(n[0] + 7) / 8, (n[1] + 7) / 8, (n[2] + 7) / 8};
    for (int tz = 0; tz < t[2]; ++tz) {
        for (int ty = 0; ty < t[1]; ++ty) {
            for (int tx = 0; tx < t[0]; ++tx) {
                std::vector<float> values;
                bool near = false;
                for (int z = 0; z < 8; ++z) {
                    for (int y = 0; y < 8; ++y) {
                        for (int x = 0; x < 8; ++x) {
                            const int i = 8 * tx + x, j = 8 * ty + y, k = 8 * tz + z;
                            // Beyond the grid: inside, were it read.
                            const bool real = i < n[0] && j < n[1] && k < n[2];
                            values.push_back(real ? distance(i, j, k) : -1.0f);
                            near = near || (real && values.back() < band);
                        }
                    }
                }
                if (!near) continue;
                tiled.tiles.push_back(static_cast<uint32_t>(tx + t[0] * (ty + t[1] * tz)));
                tiled.values.insert(tiled.values.end(), values.begin(), values.end());
            }
        }
    }
    CHECK(!tiled.tiles.empty());
    CHECK(tiled.tiles.size() < static_cast<size_t>(t[0] * t[1] * t[2]));
    for (const float iso : {0.0f, 0.05f, -0.1f}) {
        const auto a = volumeToMesh(tiled, iso, true), b = volumeToMesh(whole, iso, true);
        CHECK(a->pointCount() > 100);
        CHECK_EQ(a->hash(), b->hash());
    }
    // A background inside -- here above iso -- reaches the sides: as every voxel too.
    CHECK_EQ(volumeToMesh(tiled, 0.0f, false)->hash(), volumeToMesh(whole, 0.0f, false)->hash());
    // No tiles: the background alone, outside.
    TiledVolume none = tiled;
    none.tiles.clear();
    none.values.clear();
    CHECK_EQ(volumeToMesh(none, 0.0f, true)->pointCount(), size_t(0));
    // Tiles out of order, or fewer values than they need: nothing.
    TiledVolume wrong = tiled;
    std::swap(wrong.tiles.front(), wrong.tiles.back());
    CHECK_EQ(volumeToMesh(wrong, 0.0f, true)->pointCount(), size_t(0));
    wrong = tiled;
    wrong.values.pop_back();
    CHECK_EQ(volumeToMesh(wrong, 0.0f, true)->pointCount(), size_t(0));
}

TEST(volume_mesh_of_tiles_deep_inside_is_that_of_every_voxel) {
    // A big ball's distance, cut off at a band: the tiles deep in it -- all
    // of it at -band, off the grid's sides, kept tiles all round -- kept
    // without their values, filled; as every voxel all the same.
    const int n = 40;
    const float voxel = 0.05f, band = 0.12f, r = 0.75f;
    const Vec3 origin(-1.0f, 0.0f, -1.0f), centre(0.0f, 1.0f, 0.0f);
    auto distance = [&](int i, int j, int k) {
        const Vec3 p = origin + Vec3((static_cast<float>(i) + 0.5f) * voxel, (static_cast<float>(j) + 0.5f) * voxel,
                                     (static_cast<float>(k) + 0.5f) * voxel);
        return std::clamp(length(p - centre) - r, -band, band);
    };
    std::vector<float> every;
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) every.push_back(distance(i, j, k));
        }
    }
    const int t = n / 8;
    std::vector<int> kind(static_cast<size_t>(t * t * t), 0);  // 0 background, 1 kept, 2 all -band
    for (int tile = 0; tile < t * t * t; ++tile) {
        bool near = false, deep = true;
        for (int q = 0; q < 512; ++q) {
            const float d = distance(8 * (tile % t) + q % 8, 8 * (tile / t % t) + q / 8 % 8, 8 * (tile / (t * t)) + q / 64);
            near = near || d < band;
            deep = deep && d == -band;
        }
        kind[static_cast<size_t>(tile)] = deep ? 2 : near ? 1 : 0;
    }
    TiledVolume tiled;
    tiled.origin = origin;
    tiled.voxel = voxel;
    tiled.res[0] = tiled.res[1] = tiled.res[2] = n;
    tiled.background = band;
    tiled.fill = -band;
    for (int tile = 0; tile < t * t * t; ++tile) {
        if (kind[static_cast<size_t>(tile)] == 0) continue;
        bool within = kind[static_cast<size_t>(tile)] == 2;
        for (int q = 0; q < 27 && within; ++q) {
            const int x = tile % t + q % 3 - 1, y = tile / t % t + q / 3 % 3 - 1, z = tile / (t * t) + q / 9 - 1;
            within = x >= 0 && y >= 0 && z >= 0 && x < t && y < t && z < t && kind[static_cast<size_t>(x + t * (y + t * z))] != 0;
        }
        if (within) {
            tiled.filled.push_back(static_cast<uint32_t>(tile));
            continue;
        }
        tiled.tiles.push_back(static_cast<uint32_t>(tile));
        for (int q = 0; q < 512; ++q) {
            tiled.values.push_back(distance(8 * (tile % t) + q % 8, 8 * (tile / t % t) + q / 8 % 8, 8 * (tile / (t * t)) + q / 64));
        }
    }
    CHECK(!tiled.filled.empty());
    const Volume whole = Volume::make("surface", origin, voxel, n, n, n, every);
    const auto mesh = volumeToMesh(tiled, 0.0f, true);
    CHECK(mesh->pointCount() > 1000);
    CHECK_EQ(mesh->hash(), volumeToMesh(whole, 0.0f, true)->hash());
    // A filled tile on a side of the grid -- here the corner tile, of the
    // background before: every voxel looked at, as a dense volume with it.
    TiledVolume corner = tiled;
    corner.filled.insert(corner.filled.begin(), 0u);
    std::vector<float> withCorner = every;
    for (int q = 0; q < 512; ++q) withCorner[static_cast<size_t>(q % 8 + n * (q / 8 % 8 + n * (q / 64)))] = -band;
    const auto cornered = volumeToMesh(corner, 0.0f, true);
    CHECK_EQ(cornered->hash(), volumeToMesh(Volume::make("surface", origin, voxel, n, n, n, withCorner), 0.0f, true)->hash());
    CHECK(cornered->pointCount() > mesh->pointCount());
    // A tile both kept and filled, or filled ones out of order: nothing.
    TiledVolume wrong = tiled;
    wrong.filled.push_back(wrong.tiles.back());
    std::sort(wrong.filled.begin(), wrong.filled.end());
    CHECK_EQ(volumeToMesh(wrong, 0.0f, true)->pointCount(), size_t(0));
    wrong = tiled;
    std::reverse(wrong.filled.begin(), wrong.filled.end());
    CHECK(wrong.filled.size() < 2 || volumeToMesh(wrong, 0.0f, true)->pointCount() == 0);
}
