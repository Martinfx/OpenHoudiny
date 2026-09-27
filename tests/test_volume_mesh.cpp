//
// Volumes to polygons (volumeToMesh, the Convert Volume node): the surface
// of a ball is a closed mesh -- each edge between two faces, turned
// alike --, round and as big as the ball, its normals outward; what fills
// the grid is closed where the grid ends; the same bits on any number of
// threads.
//
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"

#include "test_framework.h"

#include <cmath>
#include <map>
#include <utility>

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
