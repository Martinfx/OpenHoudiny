// Nodes of volumes: a volume's surface as polygons.
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Parallel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace pg {

namespace {

/// The corners of a cube of eight samples, (dx, dy, dz) as the bits of the
/// number, and its twelve edges as pairs of them.
constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                               {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

/// The samples of a volume, one more all round than it has -- outside,
/// so that what the volume holds is closed where it ends.
struct Samples {
    const Volume& volume;
    const float* values;
    float iso;
    bool below;
    int n[3];

    bool real(int i, int j, int k) const { return i >= 0 && j >= 0 && k >= 0 && i < n[0] && j < n[1] && k < n[2]; }
    float value(int i, int j, int k) const {
        return values[static_cast<size_t>(i) +
                      static_cast<size_t>(n[0]) * (static_cast<size_t>(j) + static_cast<size_t>(n[1]) * static_cast<size_t>(k))];
    }
    bool inside(int i, int j, int k) const {
        if (!real(i, j, k)) return false;
        const float v = value(i, j, k);
        return below ? v < iso : v > iso;
    }
    Vec3 centre(int i, int j, int k) const {
        const float h = volume.voxel;
        return volume.origin + Vec3((static_cast<float>(i) + 0.5f) * h, (static_cast<float>(j) + 0.5f) * h,
                                    (static_cast<float>(k) + 0.5f) * h);
    }
    /// Where the surface crosses from sample a to sample b, 0 to 1: where
    /// the values reach iso -- halfway, out to where the volume ends, for a
    /// sample beyond it.
    float crossing(const int a[3], const int b[3]) const {
        if (!real(a[0], a[1], a[2]) || !real(b[0], b[1], b[2])) return 0.5f;
        const float va = value(a[0], a[1], a[2]), vb = value(b[0], b[1], b[2]);
        if (va == vb) return 0.5f;
        return std::clamp((iso - va) / (vb - va), 0.0f, 1.0f);
    }
};

/// The points of one layer of cubes -- those with the same lowest k --
/// in the order of their cubes: which cube (i + 1 + (n0 + 1) (j + 1)) and
/// where its point is.
struct Layer {
    std::vector<uint32_t> cubes;
    std::vector<Vec3> points;
    size_t first = 0;  // the number of its first point in the mesh

    /// The mesh's number of the point of cube `c`; there is one.
    uint32_t point(uint32_t c) const {
        const auto it = std::lower_bound(cubes.begin(), cubes.end(), c);
        return static_cast<uint32_t>(first + static_cast<size_t>(it - cubes.begin()));
    }
};

/// A volume's surface as polygons (volumeToMesh): the surface nets of
/// Gibson (1998) -- a point in each cube of eight samples the surface
/// passes through, at the mean of where it crosses the cube's edges, and
/// a quad across each edge between samples it crosses, through the points
/// of the four cubes round that edge.
class Mesher {
public:
    Mesher(const Volume& volume, float iso, bool below)
        : s_{volume, volume.values->data(), iso, below, {volume.res[0], volume.res[1], volume.res[2]}} {}

    std::shared_ptr<Geometry> run() {
        const int nz = s_.n[2];
        // The cubes: from the sample before the first to the last, each way.
        layers_.resize(static_cast<size_t>(nz) + 1);
        pg::parallelFor(layers_.size(), 1, [&](size_t begin, size_t end) {
            for (size_t l = begin; l < end; ++l) pointsOf(static_cast<int>(l) - 1, layers_[l]);
        });
        size_t points = 0;
        for (Layer& layer : layers_) {
            layer.first = points;
            points += layer.points.size();
        }
        auto geo = std::make_shared<Geometry>();
        if (points == 0) return geo;
        geo->addPoints(points);
        auto P = geo->positionsForWrite();
        for (const Layer& layer : layers_) std::copy(layer.points.begin(), layer.points.end(), P.begin() + static_cast<std::ptrdiff_t>(layer.first));

        // The quads: those of the edges along x and y at each k, and along z
        // from each k to the next.
        std::vector<std::vector<std::array<uint32_t, 4>>> quads(static_cast<size_t>(nz) + 1);
        pg::parallelFor(quads.size(), 1, [&](size_t begin, size_t end) {
            for (size_t l = begin; l < end; ++l) quadsOf(static_cast<int>(l) - 1, quads[l]);
        });
        std::vector<Vec3> normal(points, Vec3());
        for (const auto& layer : quads) {
            for (const auto& q : layer) {
                geo->addPrimitive(q, true);
                // Newell's normal: as long as twice the quad's area, so bigger
                // faces count more.
                Vec3 n;
                for (int c = 0; c < 4; ++c) {
                    const Vec3& a = P[q[static_cast<size_t>(c)]];
                    const Vec3& b = P[q[static_cast<size_t>((c + 1) % 4)]];
                    n.x += (a.y - b.y) * (a.z + b.z);
                    n.y += (a.z - b.z) * (a.x + b.x);
                    n.z += (a.x - b.x) * (a.y + b.y);
                }
                for (const uint32_t p : q) normal[p] = normal[p] + n;
            }
        }
        auto N = geo->points().create("N", AttrType::Vec3).write<Vec3>();
        for (size_t p = 0; p < points; ++p) {
            const float len = length(normal[p]);
            N[p] = len > 0.0f ? normal[p] * (1.0f / len) : Vec3(0.0f, 1.0f, 0.0f);
        }
        return geo;
    }

private:
    uint32_t cubeIndex(int i, int j) const {
        return static_cast<uint32_t>(i + 1) + static_cast<uint32_t>(s_.n[0] + 1) * static_cast<uint32_t>(j + 1);
    }

    /// The points of the cubes whose lowest sample is at k.
    void pointsOf(int k, Layer& layer) const {
        const int nx = s_.n[0], ny = s_.n[1];
        for (int j = -1; j < ny; ++j) {
            for (int i = -1; i < nx; ++i) {
                int corner[8][3];
                unsigned mask = 0;
                for (int c = 0; c < 8; ++c) {
                    corner[c][0] = i + (c & 1);
                    corner[c][1] = j + ((c >> 1) & 1);
                    corner[c][2] = k + ((c >> 2) & 1);
                    if (s_.inside(corner[c][0], corner[c][1], corner[c][2])) mask |= 1u << c;
                }
                if (mask == 0 || mask == 255) continue;
                Vec3 sum;
                int count = 0;
                for (const auto& e : kEdges) {
                    const bool a = (mask >> e[0]) & 1u, b = (mask >> e[1]) & 1u;
                    if (a == b) continue;
                    const int* from = corner[e[0]];
                    const int* to = corner[e[1]];
                    const float t = s_.crossing(from, to);
                    const Vec3 p = s_.centre(from[0], from[1], from[2]);
                    const Vec3 q = s_.centre(to[0], to[1], to[2]);
                    sum = sum + p + (q - p) * t;
                    ++count;
                }
                layer.cubes.push_back(cubeIndex(i, j));
                layer.points.push_back(sum * (1.0f / static_cast<float>(count)));
            }
        }
    }

    /// The point of the cube whose lowest sample is (i, j, k).
    uint32_t pointOf(int i, int j, int k) const { return layers_[static_cast<size_t>(k + 1)].point(cubeIndex(i, j)); }

    /// The quad across the edge from sample (i, j, k) one further along
    /// `axis`, when the surface crosses it: through the cubes round the
    /// edge, turning anticlockwise seen from outside.
    void quadOf(int i, int j, int k, int axis, std::vector<std::array<uint32_t, 4>>& out) const {
        int to[3] = {i, j, k};
        ++to[axis];
        const bool from = s_.inside(i, j, k);
        if (from == s_.inside(to[0], to[1], to[2])) return;
        // Round the edge, in the plane of the two other axes (b, c): the
        // cubes at (-1, -1), (0, -1), (0, 0), (-1, 0) turn anticlockwise seen
        // from further along the axis -- outward when the inside is here.
        const int b = (axis + 1) % 3, c = (axis + 2) % 3;
        static constexpr int kRound[4][2] = {{-1, -1}, {0, -1}, {0, 0}, {-1, 0}};
        std::array<uint32_t, 4> q;
        for (int r = 0; r < 4; ++r) {
            int at[3] = {i, j, k};
            at[b] += kRound[r][0];
            at[c] += kRound[r][1];
            q[static_cast<size_t>(r)] = pointOf(at[0], at[1], at[2]);
        }
        if (!from) std::swap(q[1], q[3]);
        out.push_back(q);
    }

    /// The quads of the edges along x and y at k (a sample of the volume),
    /// and of those along z from k to k + 1.
    void quadsOf(int k, std::vector<std::array<uint32_t, 4>>& out) const {
        const int nx = s_.n[0], ny = s_.n[1];
        for (int j = -1; j < ny; ++j) {
            for (int i = -1; i < nx; ++i) {
                // Each edge once, from its lower sample; round it only cubes
                // there are -- the other two axes within the volume.
                const bool inI = i >= 0, inJ = j >= 0, inK = k >= 0;
                if (inJ && inK) quadOf(i, j, k, 0, out);
                if (inI && inK) quadOf(i, j, k, 1, out);
                if (inI && inJ) quadOf(i, j, k, 2, out);
            }
        }
    }

    Samples s_;
    std::vector<Layer> layers_;
};

/// The surface of a volume as polygons: where it crosses Iso, inside above
/// it (a density) or below it (a distance).
class ConvertVolumeNode : public Node {
public:
    explicit ConvertVolumeNode(std::string name) : Node("convertvolume", std::move(name)) {
        setInputCount(1);
        params_.setString("volume", "");
        params_.setFloat("iso", 0.1f);
        params_.setInt("inside", 0);  // 0: above Iso (a density); 1: below it (a distance)
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto out = std::make_shared<Geometry>();
        if (in.empty() || !in[0]) return out;
        const std::string name = params_.getString("volume", "");
        const float iso = params_.evalFloat("iso", ctx, 0.1f);
        const bool below = params_.evalInt("inside", ctx, 0) == 1;
        for (const Volume& v : in[0]->volumes()) {
            if (!name.empty() && v.name != name) continue;
            out->append(*volumeToMesh(v, iso, below));
            if (name.empty()) break;  // the first
        }
        return out;
    }
};

}  // namespace

std::shared_ptr<Geometry> volumeToMesh(const Volume& volume, float iso, bool insideBelow) {
    if (volume.res[0] <= 0 || volume.res[1] <= 0 || volume.res[2] <= 0 || !volume.values ||
        volume.values->size() < volume.count() || !(volume.voxel > 0.0f)) {
        return std::make_shared<Geometry>();
    }
    return Mesher(volume, iso, insideBelow).run();
}

void registerVolumeNodes() {
    NodeRegistry::instance().add("convertvolume", [](const std::string& n) { return std::make_unique<ConvertVolumeNode>(n); });
}

}  // namespace pg
