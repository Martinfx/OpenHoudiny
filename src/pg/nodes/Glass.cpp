// Glass Fracture: a pane of glass broken as glass breaks where it is struck
// -- cracks that run out straight from the blow, and cracks round it from
// one to the next: a spider's web, slivers at the middle, shards growing
// wider further out, a crack branching off where a shard grows too wide.
//
//   pane     the input: a flat solid -- a window's glass, of any outline --
//            lying as its box does (fitBox): its thinnest way is across it
//   cracks   Radials rays from Impact -- where it is struck, on the pane --
//            at even angles, each turned by up to Jitter of the way to the
//            next; round it rings of chords from ray to ray, the first First
//            from the middle, each next Growth times as far, sector by sector
//            (Jitter again: the rings do not line up across a ray, as they do
//            not in glass); a sector whose chord is Split times as long as
//            the ring is deep branches -- a ray from a point on the chord on
//            out; beyond Rings rings, or the pane, the shards run to its edge
//   shards   the pane cut by the planes through each cell's sides, square to
//            it, and closed where cut -- the cut faces in the Inside Group:
//            `piece` on the primitives and points, `glass` on the primitives
//            -- 1 the pane's faces, 2 the cracks' -- and Cd the Tint: what the
//            RBD Solver breaks apart and the renderer draws as glass
#include "pg/nodes/Nodes.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace pg {
namespace {

uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double unit(uint64_t& state) { return static_cast<double>(splitmix(state) >> 11) / static_cast<double>(1ull << 53); }

struct V2 {
    double x = 0.0, y = 0.0;
};
V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
V2 operator*(V2 a, double s) { return {a.x * s, a.y * s}; }
double len(V2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
V2 polar(double r, double angle) { return {r * std::cos(angle), r * std::sin(angle)}; }

/// A wedge of the web still to be cut into rings: between the rays at
/// angles `a` and `b`, from its inner edge, A on the one to B on the other.
struct Sector {
    double a = 0.0, b = 0.0;
    V2 A, B;
    int ring = 0;
};

class GlassFractureNode : public Node {
public:
    explicit GlassFractureNode(std::string name) : Node("glassfracture", std::move(name)) {
        setInputCount(1);
        params_.setVec3("impact", Vec3(0.0f, 1.0f, 0.0f));
        params_.setInt("radials", 14);
        params_.setFloat("first", 0.04f);
        params_.setFloat("growth", 1.45f);
        params_.setInt("rings", 10);
        params_.setFloat("jitter", 0.5f);
        params_.setFloat("split", 1.1f);
        params_.setInt("seed", 1);
        params_.setVec3("tint", Vec3(0.82f, 0.9f, 0.88f));
        params_.setString("attribute", "piece");
        params_.setString("insidegroup", "inside");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0] || in[0]->primitiveCount() == 0) return std::make_shared<Geometry>();
        const GeometryPtr mesh = in[0];
        // The pane: its thinnest way across it; the web in the plane of the
        // other two, round where it is struck.
        const OrientedBox box = fitBox(*mesh, proxyPositions(*mesh));
        int across = 0;
        for (int a = 1; a < 3; ++a) {
            if (box.size(a) < box.size(across)) across = a;
        }
        const Vec3 n = box.axis[static_cast<size_t>(across)];
        const Vec3 u = box.axis[static_cast<size_t>((across + 1) % 3)], v = box.axis[static_cast<size_t>((across + 2) % 3)];
        const Vec3 middle = box.at({0.0, 0.0, 0.0});
        const Vec3 impact = params_.evalVec3("impact", ctx, Vec3(0.0f, 1.0f, 0.0f));
        const Vec3 q = impact - n * dot(impact - middle, n);
        // Past every corner of the pane: where the outermost shards end.
        double far = 0.0;
        for (int i = 0; i < 8; ++i) {
            std::array<double, 3> along{};
            for (size_t a = 0; a < 3; ++a) along[a] = 0.5 * box.size(static_cast<int>(a)) * ((i >> a) & 1 ? 1.0 : -1.0);
            far = std::max(far, static_cast<double>(length(box.at(along) - q)));
        }
        far = 2.0 * far + 1.0;

        const int radials = std::clamp(params_.evalInt("radials", ctx, 14), 3, 256);
        const double first = std::max(static_cast<double>(params_.evalFloat("first", ctx, 0.04f)), 1e-4);
        const double growth = std::max(static_cast<double>(params_.evalFloat("growth", ctx, 1.45f)), 1.05);
        const int rings = std::clamp(params_.evalInt("rings", ctx, 10), 1, 200);
        const double jitter = std::clamp(static_cast<double>(params_.evalFloat("jitter", ctx, 0.5f)), 0.0, 1.0);
        const double split = std::max(static_cast<double>(params_.evalFloat("split", ctx, 1.1f)), 0.0);
        uint64_t state = static_cast<uint64_t>(std::max(params_.evalInt("seed", ctx, 1), 0)) * 0x9E3779B97F4A7C15ull +
                         0x2545F4914F6CDD1Dull;

        // The rays, and the sectors between them, from the middle.
        const double turn = 6.283185307179586;
        const double step = turn / radials, offset = turn * unit(state);
        std::vector<double> angles;
        for (int k = 0; k < radials; ++k) angles.push_back(offset + step * (k + 0.8 * jitter * (unit(state) - 0.5)));
        std::deque<Sector> open;
        for (int k = 0; k < radials; ++k) {
            open.push_back({angles[static_cast<size_t>(k)], k + 1 < radials ? angles[static_cast<size_t>(k + 1)] : angles[0] + turn,
                            V2{}, V2{}, 0});
        }
        // Ring by ring outwards: the cells of the web, each convex, in
        // order round the middle and out.
        std::vector<std::vector<V2>> cells;
        constexpr size_t kMaxCells = 20000;
        while (!open.empty() && cells.size() < kMaxCells) {
            const Sector s = open.front();
            open.pop_front();
            const double inner = std::max(len(s.A), len(s.B));
            double r = s.ring == 0 ? first * (1.0 + 0.6 * jitter * (unit(state) - 0.5))
                                   : inner * growth * (1.0 + 0.5 * jitter * (unit(state) - 0.5));
            r = std::max(r, inner * 1.05 + 1e-6);
            const bool last = s.ring + 1 >= rings || r >= far;
            if (last) r = far;
            const V2 A = polar(r, s.a), B = polar(r, s.b);
            std::vector<V2> cell;
            if (len(s.B - s.A) > 1e-12) cell = {s.A, s.B, B, A};
            else cell = {s.A, B, A};  // at the middle: a sliver
            cells.push_back(std::move(cell));
            if (last) continue;
            // Wider than it is deep: a crack branches off the chord.
            const double chord = len(B - A), depth = r - inner;
            if (split > 0.0 && s.ring >= 1 && chord > split * depth) {
                const V2 m = A + (B - A) * (0.5 + 0.4 * jitter * (unit(state) - 0.5));
                double c = std::atan2(m.y, m.x);
                while (c < s.a) c += turn;
                while (c > s.b) c -= turn;
                open.push_back({s.a, c, A, m, s.ring + 1});
                open.push_back({c, s.b, m, B, s.ring + 1});
            } else {
                open.push_back({s.a, s.b, A, B, s.ring + 1});
            }
        }

        // Each cell cut out of the pane by the planes through its sides.
        const std::string attribute = params_.getString("attribute", "piece");
        const std::string inside = params_.getString("insidegroup", "inside");
        const std::string cutGroup = inside.empty() ? std::string("__glass_cut") : inside;
        std::vector<std::shared_ptr<Geometry>> shards(cells.size());
        parallelFor(cells.size(), 1, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (ctx.interrupted()) return;
                std::vector<V2> cell = cells[i];
                double area = 0.0;
                for (size_t k = 0; k < cell.size(); ++k) {
                    const V2 a = cell[k], b = cell[(k + 1) % cell.size()];
                    area += a.x * b.y - b.x * a.y;
                }
                if (area < 0.0) std::reverse(cell.begin(), cell.end());
                std::shared_ptr<Geometry> piece = std::make_shared<Geometry>(*mesh);
                for (size_t k = 0; k < cell.size() && piece->primitiveCount() > 0; ++k) {
                    const V2 a = cell[k], b = cell[(k + 1) % cell.size()];
                    const V2 e = b - a;
                    if (len(e) < 1e-9) continue;
                    // Inward: to the left of a side going round anticlockwise.
                    const Vec3 dir = normalize(u * static_cast<float>(-e.y) + v * static_cast<float>(e.x));
                    const Vec3 origin = q + u * static_cast<float>(a.x) + v * static_cast<float>(a.y);
                    float least = 1e30f;
                    for (const Vec3& p : piece->positions()) least = std::min(least, dot(p - origin, dir));
                    if (least >= 0.0f) continue;  // all of it inside already
                    piece = clipGeometry(*piece, origin, dir, true, cutGroup);
                }
                shards[i] = piece;
            }
        });
        if (ctx.interrupted()) return nullptr;
        const Vec3 tint = params_.evalVec3("tint", ctx, Vec3(0.82f, 0.9f, 0.88f));
        auto out = std::make_shared<Geometry>();
        int32_t number = 0;
        for (auto& shard : shards) {
            if (!shard || shard->primitiveCount() == 0) continue;
            const Group* cut = shard->findGroup(cutGroup);
            auto glass = shard->primitives().create("glass", AttrType::Int).write<int32_t>();
            for (size_t p = 0; p < glass.size(); ++p) glass[p] = cut && cut->contains(p) ? 2 : 1;
            auto cd = shard->primitives().create("Cd", AttrType::Vec3).write<Vec3>();
            std::fill(cd.begin(), cd.end(), tint);
            if (!attribute.empty()) {
                auto pp = shard->primitives().create(attribute, AttrType::Int).write<int32_t>();
                std::fill(pp.begin(), pp.end(), number);
                auto pt = shard->points().create(attribute, AttrType::Int).write<int32_t>();
                std::fill(pt.begin(), pt.end(), number);
            }
            ++number;
            out->append(*shard);
        }
        if (inside.empty()) out->eraseGroup(cutGroup);
        else if (!out->findGroup(inside)) out->createGroup(inside, AttrClass::Primitive);
        return out;
    }
};

}  // namespace

void registerGlassNodes() {
    NodeRegistry::instance().add("glassfracture", [](const std::string& n) { return std::make_unique<GlassFractureNode>(n); });
}

}  // namespace pg
