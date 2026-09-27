// The primitive shapes: box, sphere, tube -- polygons with their faces
// turned outward, as every renderer and every simulation that asks "which
// side is inside" expects.
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace pg {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Newell's normal of the polygon through `corners`: twice its area long,
/// pointing the way its corners turn anticlockwise.
Vec3 newell(const Geometry& geo, std::span<const uint32_t> corners) {
    const auto P = geo.positions();
    Vec3 n;
    for (size_t i = 0; i < corners.size(); ++i) {
        const Vec3& a = P[corners[i]];
        const Vec3& b = P[corners[(i + 1) % corners.size()]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

/// Adds the polygon, its corners reversed if it would face against `outward`.
void addFacing(Geometry& geo, std::vector<uint32_t> corners, const Vec3& outward) {
    if (dot(newell(geo, corners), outward) < 0.0f) std::reverse(corners.begin(), corners.end());
    geo.addPrimitive(corners, true);
}

/// The middle of the polygon's corners.
Vec3 middleOf(const Geometry& geo, const std::vector<uint32_t>& corners) {
    const auto P = geo.positions();
    Vec3 m;
    for (const uint32_t c : corners) m += P[c];
    return m * (1.0f / static_cast<float>(corners.size()));
}

/// A box `size` large round `center`, each face cut into divisions x
/// divisions quads that share their points along the edges.
class BoxNode : public Node {
public:
    explicit BoxNode(std::string name) : Node("box", std::move(name)) {
        setInputCount(0);
        params_.setVec3("size", Vec3(1.0f, 1.0f, 1.0f));
        params_.setVec3("center", Vec3(0.0f, 0.0f, 0.0f));
        params_.setInt("divisions", 1);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr>) override {
        const Vec3 size = params_.evalVec3("size", ctx, Vec3(1.0f, 1.0f, 1.0f));
        const Vec3 center = params_.evalVec3("center", ctx, Vec3());
        const int d = std::clamp(params_.getInt("divisions", 1), 1, 100);
        auto geo = std::make_shared<Geometry>();

        // The points of the lattice (i, j, k) in [0, d]^3 on the surface, made
        // as the faces first reach them: welded, in a fixed order.
        std::map<int, uint32_t> made;
        std::vector<Vec3> at;
        auto point = [&](int i, int j, int k) {
            const int key = (i * (d + 1) + j) * (d + 1) + k;
            const auto it = made.find(key);
            if (it != made.end()) return it->second;
            const float s = 1.0f / static_cast<float>(d);
            at.push_back(center + Vec3((static_cast<float>(i) * s - 0.5f) * size.x, (static_cast<float>(j) * s - 0.5f) * size.y,
                                       (static_cast<float>(k) * s - 0.5f) * size.z));
            const uint32_t index = static_cast<uint32_t>(at.size() - 1);
            made.emplace(key, index);
            return index;
        };
        struct Quad {
            std::vector<uint32_t> corners;
            Vec3 outward;
        };
        std::vector<Quad> quads;
        // The six faces: the axis across it, at 0 or d.
        for (int axis = 0; axis < 3; ++axis) {
            for (const int side : {0, d}) {
                Vec3 outward;
                outward[axis] = side == 0 ? -1.0f : 1.0f;
                const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                for (int a = 0; a < d; ++a) {
                    for (int b = 0; b < d; ++b) {
                        std::vector<uint32_t> corners;
                        for (const auto& [da, db] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{1, 1}, std::pair{0, 1}}) {
                            int c[3];
                            c[axis] = side;
                            c[u] = a + da;
                            c[v] = b + db;
                            corners.push_back(point(c[0], c[1], c[2]));
                        }
                        quads.push_back({std::move(corners), outward});
                    }
                }
            }
        }
        geo->addPoints(at.size());
        std::copy(at.begin(), at.end(), geo->positionsForWrite().begin());
        for (Quad& q : quads) addFacing(*geo, std::move(q.corners), q.outward);
        return geo;
    }
};

/// A sphere of `radius` round `center`: `rows` bands from pole to pole,
/// `columns` round it -- quads, and triangles at the poles.
class SphereNode : public Node {
public:
    explicit SphereNode(std::string name) : Node("sphere", std::move(name)) {
        setInputCount(0);
        params_.setFloat("radius", 0.5f);
        params_.setVec3("center", Vec3(0.0f, 0.0f, 0.0f));
        params_.setInt("rows", 12);
        params_.setInt("columns", 24);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr>) override {
        const float r = params_.evalFloat("radius", ctx, 0.5f);
        const Vec3 center = params_.evalVec3("center", ctx, Vec3());
        const int rows = std::clamp(params_.getInt("rows", 12), 3, 1000);
        const int cols = std::clamp(params_.getInt("columns", 24), 3, 1000);
        auto geo = std::make_shared<Geometry>();
        // The north pole, the rings between, the south pole.
        const size_t ringPoints = static_cast<size_t>(rows - 1) * static_cast<size_t>(cols);
        geo->addPoints(ringPoints + 2);
        auto P = geo->positionsForWrite();
        P[0] = center + Vec3(0.0f, r, 0.0f);
        for (int t = 1; t < rows; ++t) {
            const float theta = kPi * static_cast<float>(t) / static_cast<float>(rows);
            for (int s = 0; s < cols; ++s) {
                const float phi = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(cols);
                P[1 + static_cast<size_t>(t - 1) * static_cast<size_t>(cols) + static_cast<size_t>(s)] =
                    center + Vec3(r * std::sin(theta) * std::cos(phi), r * std::cos(theta), r * std::sin(theta) * std::sin(phi));
            }
        }
        const uint32_t south = static_cast<uint32_t>(ringPoints + 1);
        P[south] = center + Vec3(0.0f, -r, 0.0f);
        auto ring = [&](int t, int s) {
            return static_cast<uint32_t>(1 + static_cast<size_t>(t - 1) * static_cast<size_t>(cols) + static_cast<size_t>(s % cols));
        };
        auto add = [&](std::vector<uint32_t> corners) {
            const Vec3 m = middleOf(*geo, corners);
            addFacing(*geo, std::move(corners), m - center);
        };
        for (int s = 0; s < cols; ++s) add({0, ring(1, s), ring(1, s + 1)});
        for (int t = 1; t + 1 < rows; ++t) {
            for (int s = 0; s < cols; ++s) add({ring(t, s), ring(t + 1, s), ring(t + 1, s + 1), ring(t, s + 1)});
        }
        for (int s = 0; s < cols; ++s) add({ring(rows - 1, s), south, ring(rows - 1, s + 1)});
        return geo;
    }
};

/// A tube standing along y round `center`: `columns` round it, `rows` up
/// it, closed at both ends by a polygon each when `caps` is on.
class TubeNode : public Node {
public:
    explicit TubeNode(std::string name) : Node("tube", std::move(name)) {
        setInputCount(0);
        params_.setFloat("radius", 0.3f);
        params_.setFloat("height", 1.0f);
        params_.setVec3("center", Vec3(0.0f, 0.0f, 0.0f));
        params_.setInt("columns", 24);
        params_.setInt("rows", 1);
        params_.setBool("caps", true);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr>) override {
        const float r = params_.evalFloat("radius", ctx, 0.3f);
        const float h = params_.evalFloat("height", ctx, 1.0f);
        const Vec3 center = params_.evalVec3("center", ctx, Vec3());
        const int cols = std::clamp(params_.getInt("columns", 24), 3, 1000);
        const int rows = std::clamp(params_.getInt("rows", 1), 1, 1000);
        auto geo = std::make_shared<Geometry>();
        geo->addPoints(static_cast<size_t>(rows + 1) * static_cast<size_t>(cols));
        auto P = geo->positionsForWrite();
        for (int j = 0; j <= rows; ++j) {
            const float y = -0.5f * h + h * static_cast<float>(j) / static_cast<float>(rows);
            for (int s = 0; s < cols; ++s) {
                const float phi = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(cols);
                P[static_cast<size_t>(j) * static_cast<size_t>(cols) + static_cast<size_t>(s)] =
                    center + Vec3(r * std::cos(phi), y, r * std::sin(phi));
            }
        }
        auto at = [&](int j, int s) { return static_cast<uint32_t>(j * cols + s % cols); };
        for (int j = 0; j < rows; ++j) {
            for (int s = 0; s < cols; ++s) {
                std::vector<uint32_t> q = {at(j, s), at(j + 1, s), at(j + 1, s + 1), at(j, s + 1)};
                const Vec3 m = middleOf(*geo, q);
                addFacing(*geo, std::move(q), Vec3(m.x - center.x, 0.0f, m.z - center.z));
            }
        }
        if (params_.getBool("caps", true)) {
            std::vector<uint32_t> bottom, top;
            for (int s = 0; s < cols; ++s) {
                bottom.push_back(at(0, s));
                top.push_back(at(rows, s));
            }
            addFacing(*geo, std::move(bottom), Vec3(0.0f, -1.0f, 0.0f));
            addFacing(*geo, std::move(top), Vec3(0.0f, 1.0f, 0.0f));
        }
        return geo;
    }
};

}  // namespace

void registerPrimitiveNodes() {
    auto& r = NodeRegistry::instance();
    r.add("box", [](const std::string& n) { return std::make_unique<BoxNode>(n); });
    r.add("sphere", [](const std::string& n) { return std::make_unique<SphereNode>(n); });
    r.add("tube", [](const std::string& n) { return std::make_unique<TubeNode>(n); });
}

/// Newell's normal, for the other nodes: twice the polygon's area long.
Vec3 polygonNormal(const Geometry& geo, std::span<const uint32_t> corners) { return newell(geo, corners); }

}  // namespace pg
