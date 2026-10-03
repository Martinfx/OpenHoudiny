// Nodes of volumes: a volume's surface as polygons.
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Parallel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <vector>

namespace pg {

namespace {

/// The corners of a cube of eight samples, (dx, dy, dz) as the bits of the
/// number, and its twelve edges as pairs of them.
constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                               {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

/// The values of a volume with every voxel kept, x fastest.
struct DenseValues {
    const float* values;
    int n[3];

    float operator()(int i, int j, int k) const {
        return values[static_cast<size_t>(i) +
                      static_cast<size_t>(n[0]) * (static_cast<size_t>(j) + static_cast<size_t>(n[1]) * static_cast<size_t>(k))];
    }
    /// f(i, j) for the cubes of layer k the surface may pass through, in
    /// order, j slowest: every one of them.
    template <class F>
    void forCubes(int, const F& f) const {
        for (int j = -1; j < n[1]; ++j) {
            for (int i = -1; i < n[0]; ++i) f(i, j);
        }
    }
};

/// The values of a TiledVolume whose background is outside: those of its
/// tiles, its fill in the filled ones, the background in the others.
class TiledValues {
public:
    static constexpr int kLog = 3, kSide = 8, kCells = 512;
    static constexpr int32_t kBackground = -1, kFilled = -2;

    explicit TiledValues(const TiledVolume& v)
        : values_(v.values.data()), tiles_(v.tiles), background_(v.background), fill_(v.fill) {
        for (int a = 0; a < 3; ++a) {
            n_[a] = v.res[a];
            t_[a] = (v.res[a] + kSide - 1) >> kLog;
        }
        slot_.assign(static_cast<size_t>(t_[0]) * static_cast<size_t>(t_[1]) * static_cast<size_t>(t_[2]), kBackground);
        for (size_t s = 0; s < tiles_.size(); ++s) slot_[tiles_[s]] = static_cast<int32_t>(s);
        for (const uint32_t t : v.filled) slot_[t] = kFilled;
    }

    float operator()(int i, int j, int k) const {
        const int32_t s = slot_[static_cast<size_t>(i >> kLog) +
                                static_cast<size_t>(t_[0]) * (static_cast<size_t>(j >> kLog) +
                                                              static_cast<size_t>(t_[1]) * static_cast<size_t>(k >> kLog))];
        if (s < 0) return s == kFilled ? fill_ : background_;
        return values_[static_cast<size_t>(s) * kCells + static_cast<size_t>(i & (kSide - 1)) +
                       kSide * (static_cast<size_t>(j & (kSide - 1)) + kSide * static_cast<size_t>(k & (kSide - 1)))];
    }

    /// Is each of `filled` off the sides of the volume, with kept or filled
    /// tiles all round it? Then no cube the surface passes through has a
    /// sample in it but those that have one in a kept tile too.
    bool filledWithin(const std::vector<uint32_t>& filled) const {
        const size_t tx = static_cast<size_t>(t_[0]), txy = tx * static_cast<size_t>(t_[1]);
        for (const uint32_t t : filled) {
            const int c[3] = {static_cast<int>(t % tx), static_cast<int>(t / tx % static_cast<size_t>(t_[1])),
                              static_cast<int>(t / txy)};
            for (int q = 0; q < 27; ++q) {
                const int x = c[0] + q % 3 - 1, y = c[1] + q / 3 % 3 - 1, z = c[2] + q / 9 - 1;
                if (x < 0 || y < 0 || z < 0 || x >= t_[0] || y >= t_[1] || z >= t_[2]) return false;
                if (slot_[static_cast<size_t>(x) + tx * static_cast<size_t>(y) + txy * static_cast<size_t>(z)] == kBackground) {
                    return false;
                }
            }
        }
        return true;
    }

    /// f(i, j) for the cubes of layer k the surface may pass through, in the
    /// order of DenseValues': those with a sample in a kept tile. A cube
    /// whose samples are all in tiles not kept, or beyond the volume, is
    /// outside through and through.
    template <class F>
    void forCubes(int k, const F& f) const {
        // The kept tiles of the layer's two planes of samples, k and k + 1,
        // as tx + t_[0] * ty, in order.
        std::vector<uint32_t> columns;
        const size_t plane = static_cast<size_t>(t_[0]) * static_cast<size_t>(t_[1]);
        int planes[2] = {k >= 0 ? k >> kLog : -1, k + 1 < n_[2] ? (k + 1) >> kLog : -1};
        if (planes[1] == planes[0]) planes[1] = -1;
        for (const int tz : planes) {
            if (tz < 0) continue;
            const auto first = std::lower_bound(tiles_.begin(), tiles_.end(), static_cast<uint32_t>(plane * static_cast<size_t>(tz)));
            const auto last = std::lower_bound(first, tiles_.end(), static_cast<uint32_t>(plane * static_cast<size_t>(tz + 1)));
            const size_t had = columns.size();
            for (auto t = first; t != last; ++t) columns.push_back(static_cast<uint32_t>(*t - plane * static_cast<size_t>(tz)));
            std::inplace_merge(columns.begin(), columns.begin() + static_cast<std::ptrdiff_t>(had), columns.end());
        }
        columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
        if (columns.empty()) return;
        // The kept tiles of row ty of tiles, as a range of `columns`.
        auto row = [&](int ty, const uint32_t*& from, const uint32_t*& to) {
            from = to = nullptr;
            if (ty < 0) return;
            const auto a = std::lower_bound(columns.begin(), columns.end(), static_cast<uint32_t>(t_[0]) * static_cast<uint32_t>(ty));
            const auto b = std::lower_bound(a, columns.end(), static_cast<uint32_t>(t_[0]) * static_cast<uint32_t>(ty + 1));
            if (a == b) return;
            from = columns.data() + (a - columns.begin());
            to = columns.data() + (b - columns.begin());
        };
        // The rows of cubes with a sample in a kept tile: those from the
        // sample before a row of tiles to its last.
        std::vector<uint8_t> rows(static_cast<size_t>(n_[1]) + 1, 0);
        for (const uint32_t c : columns) {
            const int ty = static_cast<int>(c / static_cast<uint32_t>(t_[0]));
            for (int j = ty * kSide - 1; j < std::min(ty * kSide + kSide, n_[1]); ++j) rows[static_cast<size_t>(j + 1)] = 1;
        }
        for (int j = -1; j < n_[1]; ++j) {
            if (!rows[static_cast<size_t>(j + 1)]) continue;
            // The tiles of the cubes' two rows of samples, j and j + 1.
            const uint32_t *a, *aEnd, *b, *bEnd;
            row(j >= 0 ? j >> kLog : -1, a, aEnd);
            row(j + 1 < n_[1] && (j + 1) >> kLog != (j >= 0 ? j >> kLog : -1) ? (j + 1) >> kLog : -1, b, bEnd);
            int next = -1;  // the first cube not yet given
            while (a != aEnd || b != bEnd) {
                int tx;
                if (b == bEnd || (a != aEnd && *a % static_cast<uint32_t>(t_[0]) <= *b % static_cast<uint32_t>(t_[0]))) {
                    tx = static_cast<int>(*a++ % static_cast<uint32_t>(t_[0]));
                } else {
                    tx = static_cast<int>(*b++ % static_cast<uint32_t>(t_[0]));
                }
                // The cubes with a sample in the tile: from the sample before it.
                for (int i = std::max(tx * kSide - 1, next); i < std::min(tx * kSide + kSide, n_[0]); ++i) f(i, j);
                next = std::max(next, std::min(tx * kSide + kSide, n_[0]));
            }
        }
    }

private:
    const float* values_;
    const std::vector<uint32_t>& tiles_;
    float background_, fill_;
    int n_[3], t_[3];
    std::vector<int32_t> slot_;  // per tile of the volume, its place in tiles_, else kBackground or kFilled
};

/// The samples of a volume, one more all round than it has -- outside,
/// so that what the volume holds is closed where it ends.
template <class Values>
struct Samples {
    Vec3 origin;
    float voxel;
    Values values;
    float iso;
    bool below;
    int n[3];

    bool real(int i, int j, int k) const { return i >= 0 && j >= 0 && k >= 0 && i < n[0] && j < n[1] && k < n[2]; }
    float value(int i, int j, int k) const { return values(i, j, k); }
    bool inside(int i, int j, int k) const {
        if (!real(i, j, k)) return false;
        const float v = value(i, j, k);
        return below ? v < iso : v > iso;
    }
    Vec3 centre(int i, int j, int k) const {
        const float h = voxel;
        return origin + Vec3((static_cast<float>(i) + 0.5f) * h, (static_cast<float>(j) + 0.5f) * h,
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
template <class Values>
class Mesher {
public:
    Mesher(const Vec3& origin, float voxel, Values values, float iso, bool below, const int res[3])
        : s_{origin, voxel, std::move(values), iso, below, {res[0], res[1], res[2]}} {}

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
        s_.values.forCubes(k, [&](int i, int j) {
            int corner[8][3];
            unsigned mask = 0;
            for (int c = 0; c < 8; ++c) {
                corner[c][0] = i + (c & 1);
                corner[c][1] = j + ((c >> 1) & 1);
                corner[c][2] = k + ((c >> 2) & 1);
                if (s_.inside(corner[c][0], corner[c][1], corner[c][2])) mask |= 1u << c;
            }
            if (mask == 0 || mask == 255) return;
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
        });
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
    /// and of those along z from k to k + 1: of the samples that are the
    /// lowest of a cube the surface may pass through -- an edge it crosses
    /// is one of such a cube's.
    void quadsOf(int k, std::vector<std::array<uint32_t, 4>>& out) const {
        s_.values.forCubes(k, [&](int i, int j) {
            // Each edge once, from its lower sample; round it only cubes
            // there are -- the other two axes within the volume.
            const bool inI = i >= 0, inJ = j >= 0, inK = k >= 0;
            if (inJ && inK) quadOf(i, j, k, 0, out);
            if (inI && inK) quadOf(i, j, k, 1, out);
            if (inI && inJ) quadOf(i, j, k, 2, out);
        });
    }

    Samples<Values> s_;
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
    return Mesher<DenseValues>(volume.origin, volume.voxel, DenseValues{volume.values->data(), {volume.res[0], volume.res[1], volume.res[2]}},
                               iso, insideBelow, volume.res)
        .run();
}

std::shared_ptr<Geometry> volumeToMesh(const TiledVolume& volume, float iso, bool insideBelow) {
    constexpr int kSide = TiledValues::kSide;
    const int* n = volume.res;
    if (n[0] <= 0 || n[1] <= 0 || n[2] <= 0 || !(volume.voxel > 0.0f) ||
        volume.values.size() < volume.tiles.size() * static_cast<size_t>(TiledValues::kCells)) {
        return std::make_shared<Geometry>();
    }
    const size_t tiles = static_cast<size_t>((n[0] + kSide - 1) / kSide) * static_cast<size_t>((n[1] + kSide - 1) / kSide) *
                         static_cast<size_t>((n[2] + kSide - 1) / kSide);
    // Tiles of the volume, in order, each once; none both kept and filled.
    auto inOrder = [&](const std::vector<uint32_t>& list) {
        for (size_t s = 0; s < list.size(); ++s) {
            if (list[s] >= tiles || (s > 0 && list[s - 1] >= list[s])) return false;
        }
        return true;
    };
    std::vector<uint32_t> both;
    std::set_intersection(volume.tiles.begin(), volume.tiles.end(), volume.filled.begin(), volume.filled.end(),
                          std::back_inserter(both));
    if (!inOrder(volume.tiles) || !inOrder(volume.filled) || !both.empty()) return std::make_shared<Geometry>();
    TiledValues values(volume);
    const float b = volume.background;
    if ((insideBelow ? b < iso : b > iso) || !values.filledWithin(volume.filled)) {
        // A background inside meets the volume's sides everywhere, and the
        // surface may pass by a filled tile not deep in the rest: every
        // voxel, then.
        std::vector<float> all(static_cast<size_t>(n[0]) * static_cast<size_t>(n[1]) * static_cast<size_t>(n[2]));
        pg::parallelFor(static_cast<size_t>(n[2]), 1, [&](size_t begin, size_t end) {
            for (int k = static_cast<int>(begin); k < static_cast<int>(end); ++k) {
                for (int j = 0; j < n[1]; ++j) {
                    for (int i = 0; i < n[0]; ++i) {
                        all[static_cast<size_t>(i) + static_cast<size_t>(n[0]) * (static_cast<size_t>(j) + static_cast<size_t>(n[1]) * static_cast<size_t>(k))] =
                            values(i, j, k);
                    }
                }
            }
        });
        return volumeToMesh(Volume::make("tiled", volume.origin, volume.voxel, n[0], n[1], n[2], std::move(all)), iso, insideBelow);
    }
    return Mesher<TiledValues>(volume.origin, volume.voxel, std::move(values), iso, insideBelow, n).run();
}

void registerVolumeNodes() {
    NodeRegistry::instance().add("convertvolume", [](const std::string& n) { return std::make_unique<ConvertVolumeNode>(n); });
}

}  // namespace pg
