// Rebar: the steel bars inside a block of concrete, laid as they are before
// it is poured -- for the RBD Solver's Rebar input, which holds the pieces
// on them once the concrete cracks.
//
//   block   the input's box, turned as it lies: square to its biggest flat
//           side -- the faces that face one way (or the other), the most
//           area together -- and in the plane of that side the rectangle
//           round the points that fits them closest (rotating calipers
//           round their hull); along the world's axes when that box is as
//           small. As the proxy has it, where the input carries one
//           (Concrete Fracture's): the pieces of a fracture as well as the
//           block they were cut from
//   mesh    a wall or a slab -- its thinnest side under half the next: bars
//           both ways along it, Spacing apart, a layer near each face
//           (Layers 2) or one in the middle, the bars of one way lying on
//           those of the other; Cover of concrete over them and past their
//           ends
//   cage    a beam or a column: bars along it round the edge of its cross
//           section -- one in each corner, no more than Spacing apart -- and
//           stirrups round them, Spacing apart along it
//   bars    open polylines -- a stirrup ends where it starts -- with the
//           point attribute width: their diameter
#include "pg/nodes/Nodes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>

namespace pg {
namespace {

using D2 = std::array<double, 2>;

double turn(const D2& o, const D2& a, const D2& b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

/// The convex hull of `p`, anticlockwise (Andrew's monotone chain).
std::vector<D2> hull2(std::vector<D2> p) {
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end()), p.end());
    if (p.size() < 3) return p;
    std::vector<D2> h(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        while (k >= 2 && turn(h[k - 2], h[k - 1], p[i]) <= 0.0) --k;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && turn(h[k - 2], h[k - 1], p[i]) <= 0.0) --k;
        h[k++] = p[i];
    }
    h.resize(k - 1);
    return h;
}

/// A box turned as `axis` say, round points.
struct Block {
    std::array<Vec3, 3> axis{Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)};
    std::array<double, 3> lo{0.0, 0.0, 0.0}, hi{0.0, 0.0, 0.0};  ///< how far along each axis the points go

    double size(int a) const { return hi[static_cast<size_t>(a)] - lo[static_cast<size_t>(a)]; }
    double volume() const { return size(0) * size(1) * size(2); }
    /// The point `along` each axis from the box's middle.
    Vec3 at(const std::array<double, 3>& along) const {
        Vec3 p;
        for (size_t a = 0; a < 3; ++a) p += axis[a] * static_cast<float>(0.5 * (lo[a] + hi[a]) + along[a]);
        return p;
    }
};

Block blockAlong(const std::vector<Vec3>& P, const std::array<Vec3, 3>& axis) {
    Block b;
    b.axis = axis;
    b.lo = {1e300, 1e300, 1e300};
    b.hi = {-1e300, -1e300, -1e300};
    for (const Vec3& p : P) {
        for (size_t a = 0; a < 3; ++a) {
            const double d = static_cast<double>(p.x) * axis[a].x + static_cast<double>(p.y) * axis[a].y +
                             static_cast<double>(p.z) * axis[a].z;
            b.lo[a] = std::min(b.lo[a], d);
            b.hi[a] = std::max(b.hi[a], d);
        }
    }
    return b;
}

/// The box round the points `P` of `geo`, turned as it lies.
Block fit(const Geometry& geo, const std::vector<Vec3>& P) {
    const Block world = blockAlong(P, {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)});
    // The faces, as the way they face -- one way or the other -- and how big.
    struct Side {
        std::array<double, 3> n;
        double area;
    };
    std::vector<Side> faces;
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto c = geo.primitivePoints(prim);
        if (c.size() < 3 || !geo.primitiveClosed(prim)) continue;
        std::array<double, 3> n{0.0, 0.0, 0.0};
        for (size_t i = 0; i < c.size(); ++i) {
            const Vec3& a = P[c[i]];
            const Vec3& b = P[c[(i + 1) % c.size()]];
            n[0] += (static_cast<double>(a.y) - b.y) * (static_cast<double>(a.z) + b.z);
            n[1] += (static_cast<double>(a.z) - b.z) * (static_cast<double>(a.x) + b.x);
            n[2] += (static_cast<double>(a.x) - b.x) * (static_cast<double>(a.y) + b.y);
        }
        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-18) continue;
        size_t big = 0;
        for (size_t a = 1; a < 3; ++a) {
            if (std::fabs(n[a]) > std::fabs(n[big])) big = a;
        }
        const double sign = n[big] < 0.0 ? -1.0 : 1.0;
        faces.push_back({{sign * n[0] / len, sign * n[1] / len, sign * n[2] / len}, 0.5 * len});
    }
    if (faces.empty()) return world;
    std::map<std::tuple<long, long, long>, double> areas;
    for (const Side& f : faces) {
        areas[{std::lround(f.n[0] * 256.0), std::lround(f.n[1] * 256.0), std::lround(f.n[2] * 256.0)}] += f.area;
    }
    const auto most = std::max_element(areas.begin(), areas.end(),
                                       [](const auto& a, const auto& b) { return a.second < b.second; });
    // The faces within a degree of that way, together.
    const std::array<double, 3> rough{std::get<0>(most->first) / 256.0, std::get<1>(most->first) / 256.0,
                                      std::get<2>(most->first) / 256.0};
    std::array<double, 3> sum{0.0, 0.0, 0.0};
    const double rlen = std::sqrt(rough[0] * rough[0] + rough[1] * rough[1] + rough[2] * rough[2]);
    for (const Side& f : faces) {
        const double c = (f.n[0] * rough[0] + f.n[1] * rough[1] + f.n[2] * rough[2]) / rlen;
        if (std::fabs(c) < 0.9998) continue;
        const double s = c < 0.0 ? -f.area : f.area;
        for (size_t a = 0; a < 3; ++a) sum[a] += s * f.n[a];
    }
    const Vec3 n = normalize(Vec3(static_cast<float>(sum[0]), static_cast<float>(sum[1]), static_cast<float>(sum[2])));
    if (length(n) < 0.5f) return world;
    // In the plane of that side: the rectangle round the points that fits
    // them closest has a side along an edge of their hull.
    const Vec3 u = normalize(cross(n, std::fabs(n.x) < 0.6f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f)));
    const Vec3 v = cross(n, u);
    std::vector<D2> flat;
    flat.reserve(P.size());
    for (const Vec3& p : P) flat.push_back({static_cast<double>(dot(p, u)), static_cast<double>(dot(p, v))});
    const std::vector<D2> h = hull2(std::move(flat));
    if (h.size() < 3) return world;
    double best = 1e300;
    D2 along{1.0, 0.0};
    for (size_t i = 0; i < h.size(); ++i) {
        const D2& a = h[i];
        const D2& b = h[(i + 1) % h.size()];
        const double ex = b[0] - a[0], ey = b[1] - a[1], len = std::sqrt(ex * ex + ey * ey);
        if (len < 1e-12) continue;
        const D2 d{ex / len, ey / len};
        double lo0 = 1e300, hi0 = -1e300, lo1 = 1e300, hi1 = -1e300;
        for (const D2& q : h) {
            const double s = q[0] * d[0] + q[1] * d[1], t = -q[0] * d[1] + q[1] * d[0];
            lo0 = std::min(lo0, s);
            hi0 = std::max(hi0, s);
            lo1 = std::min(lo1, t);
            hi1 = std::max(hi1, t);
        }
        const double area = (hi0 - lo0) * (hi1 - lo1);
        if (area < best) {
            best = area;
            along = d;
        }
    }
    const Vec3 a = normalize(u * static_cast<float>(along[0]) + v * static_cast<float>(along[1]));
    const Block turned = blockAlong(P, {a, cross(n, a), n});
    return turned.volume() < world.volume() * (1.0 - 1e-4) ? turned : world;
}

/// `n` places evenly from -half to half: the middle for one.
std::vector<double> spaced(double half, int n) {
    std::vector<double> out;
    if (n <= 1) return {0.0};
    for (int i = 0; i < n; ++i) out.push_back(-half + 2.0 * half * static_cast<double>(i) / static_cast<double>(n - 1));
    return out;
}

/// How many bars from -half to half, no more than `spacing` apart.
int barsAcross(double half, double spacing) {
    if (half <= 0.0) return 1;
    return static_cast<int>(std::ceil(2.0 * half / spacing - 1e-6)) + 1;
}

class RebarNode : public Node {
public:
    explicit RebarNode(std::string name) : Node("rebar", std::move(name)) {
        setInputCount(1);
        params_.setInt("layout", 0);  // 0 as the block is, 1 mesh, 2 cage
        params_.setFloat("spacing", 0.2f);
        params_.setFloat("cover", 0.035f);
        params_.setFloat("diameter", 0.012f);
        params_.setInt("layers", 2);
        params_.setFloat("stirrup", 0.008f);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto out = std::make_shared<Geometry>();
        const GeometryPtr src = in.empty() ? nullptr : in[0];
        if (!src || src->pointCount() == 0) return out;
        // As the proxy has it, where there is one.
        const auto Pin = src->positions();
        std::vector<Vec3> P(Pin.begin(), Pin.end());
        if (const AttributeArray* proxy = src->points().find("proxy");
            proxy && proxy->type() == AttrType::Vec3 && proxy->size() == P.size()) {
            const auto Q = proxy->read<Vec3>();
            for (size_t i = 0; i < P.size(); ++i) {
                if (!(Q[i] == Vec3() && length(P[i]) > 1e-3f)) P[i] = Q[i];
            }
        }
        const Block block = fit(*src, P);
        // Its sides, thinnest first.
        std::array<int, 3> order{0, 1, 2};
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return block.size(a) < block.size(b) || (block.size(a) == block.size(b) && a < b);
        });
        const double spacing = std::max(static_cast<double>(params_.evalFloat("spacing", ctx, 0.2f)), 0.01);
        const double cover = std::max(static_cast<double>(params_.evalFloat("cover", ctx, 0.035f)), 0.0);
        const double d = std::max(static_cast<double>(params_.evalFloat("diameter", ctx, 0.012f)), 1e-4);
        const double ds = std::max(static_cast<double>(params_.evalFloat("stirrup", ctx, 0.008f)), 0.0);
        const int layers = std::clamp(params_.evalInt("layers", ctx, 2), 1, 2);
        int layout = params_.evalInt("layout", ctx, 0);
        const double h0 = 0.5 * block.size(order[0]), h1 = 0.5 * block.size(order[1]), h2 = 0.5 * block.size(order[2]);
        if (layout != 1 && layout != 2) layout = h0 < 0.5 * h1 ? 1 : 2;

        std::vector<Vec3> points;
        std::vector<float> widths;
        std::vector<std::vector<uint32_t>> bars;
        // A bar through the places `along` the block's axes (thinnest,
        // next, longest) from its middle.
        auto bar = [&](const std::vector<std::array<double, 3>>& at, double width) {
            std::vector<uint32_t> ids;
            for (const std::array<double, 3>& a : at) {
                std::array<double, 3> along{0.0, 0.0, 0.0};
                for (size_t k = 0; k < 3; ++k) along[static_cast<size_t>(order[k])] = a[k];
                ids.push_back(static_cast<uint32_t>(points.size()));
                points.push_back(block.at(along));
                widths.push_back(static_cast<float>(width));
            }
            bars.push_back(std::move(ids));
        };
        if (layout == 1) {
            // A mesh: bars the long way lie outside those the other way.
            const double end1 = h1 - cover, end2 = h2 - cover;
            if (end1 > 0.0 && end2 > 0.0) {
                std::vector<std::pair<double, double>> depths;  // of the bars the long way, and the other way
                const double outer = h0 - cover - 0.5 * d, inner = outer - d;
                if (layers == 2 && inner > d) {
                    depths = {{-outer, -inner}, {outer, inner}};
                } else {
                    depths = {{0.5 * d, -0.5 * d}};
                }
                for (const auto& [longWay, otherWay] : depths) {
                    for (const double y : spaced(end1 - 0.5 * d, barsAcross(end1 - 0.5 * d, spacing))) {
                        bar({{longWay, y, -end2}, {longWay, y, end2}}, d);
                    }
                    for (const double z : spaced(end2 - 0.5 * d, barsAcross(end2 - 0.5 * d, spacing))) {
                        bar({{otherWay, -end1, z}, {otherWay, end1, z}}, d);
                    }
                }
            }
        } else {
            // A cage: bars along it round the edge of its cross section,
            // inside the stirrups.
            const double r0 = std::max(h0 - cover - ds - 0.5 * d, 0.0), r1 = std::max(h1 - cover - ds - 0.5 * d, 0.0);
            const double end = h2 - cover;
            if (end > 0.0) {
                std::vector<std::pair<double, double>> round;
                const int n0 = r0 > 0.0 ? barsAcross(r0, spacing) : 1, n1 = r1 > 0.0 ? barsAcross(r1, spacing) : 1;
                const std::vector<double> across0 = spaced(r0, n0), across1 = spaced(r1, n1);
                // Round the rectangle: along one side, back along the other,
                // and the rows between them at their ends.
                for (const double y : across1) round.push_back({-r0, y});
                if (n0 > 1) {
                    for (const double y : across1) round.push_back({r0, y});
                }
                if (n1 > 1) {
                    for (size_t i = 1; i + 1 < across0.size(); ++i) {
                        round.push_back({across0[i], -r1});
                        round.push_back({across0[i], r1});
                    }
                }
                for (const auto& [x, y] : round) bar({{x, y, -end}, {x, y, end}}, d);
                // The stirrups: closed rectangles round the bars.
                const double s0 = h0 - cover - 0.5 * ds, s1 = h1 - cover - 0.5 * ds;
                if (ds > 0.0 && s0 > 0.0 && s1 > 0.0) {
                    const double run = end - 0.5 * ds;
                    for (const double z : spaced(run, barsAcross(run, spacing))) {
                        bar({{-s0, -s1, z}, {s0, -s1, z}, {s0, s1, z}, {-s0, s1, z}, {-s0, -s1, z}}, ds);
                    }
                }
            }
        }
        out->addPoints(points.size());
        auto Pout = out->positionsForWrite();
        std::copy(points.begin(), points.end(), Pout.begin());
        auto width = out->points().create("width", AttrType::Float).write<float>();
        std::copy(widths.begin(), widths.end(), width.begin());
        for (const std::vector<uint32_t>& ids : bars) out->addPrimitive(ids, false);
        return out;
    }
};

}  // namespace

void registerRebarNodes() {
    NodeRegistry::instance().add("rebar", [](const std::string& n) { return std::make_unique<RebarNode>(n); });
}

}  // namespace pg
