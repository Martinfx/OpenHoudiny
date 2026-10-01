// Brick Wall: a wall laid of bricks as a bricklayer lays it -- course on
// course in a bond, each brick on its bed of mortar with a joint at its end
// -- filling the input: a solid of any outline, openings and all, standing
// as it stands.
//
//   the wall   the input's box (fitBox): up the way of it nearest the
//              world's up, across the thinnest of the other two -- its front
//              the face towards Front -- along the third. As many courses as
//              fit its height, the beds a hair thicker or thinner so that
//              they fill it; across, as many leaves of stretchers as fit,
//              with a joint between them, or a header right across
//   bond       where the bricks of each course break joint: Stretcher --
//              every course half a brick on from the one below; English -- a
//              course of headers, a course of stretchers a quarter brick on;
//              Flemish -- header and stretcher in turn in each course, each
//              header over the middle of a stretcher; Stack -- joint over
//              joint; Auto -- Stretcher for a wall one leaf thick, else
//              English
//   openings   each course is laid along the stretches of it inside the
//              input -- a closed solid, or several that touch -- through the
//              middle of the wall: bricks cut short where a stretch ends, a
//              sliver too short to be a brick given to the brick beside it
//   bricks     each a closed box with its mortar -- the bed under it, the
//              joint at its end, the joint behind it where another leaf is --
//              and the Plaster on the wall's faces in front of it: one piece,
//              piece its number, Cd the brick's colour (each a shade of its
//              own), the mortar's, the plaster's -- material brick, mortar,
//              plaster. Broken of those half as
//              long again as they are wide are cut in two across, the halves
//              one cluster held Strength times as hard as the mortar
//              (clusterglue): a hard knock breaks them, the faces of the
//              break the brick's colour inside. Link them into an RBD Solver:
//              its glue is the mortar
#include "pg/nodes/Nodes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace pg {
namespace {

uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double unit(uint64_t& state) { return static_cast<double>(splitmix(state) >> 11) / static_cast<double>(1ull << 53); }

/// `state` and `v` mixed: a state of its own for each v.
uint64_t mix(uint64_t state, int v) {
    state ^= static_cast<uint64_t>(static_cast<uint32_t>(v)) * 0xD6E8FEB86659FD93ull;
    return splitmix(state);
}

/// The input as faces to find where a line runs inside it: each closed
/// polygon's plane -- its normal out of the solid, as its corners turn --
/// and its corners.
class Solid {
public:
    explicit Solid(const Geometry& geo) {
        const auto P = geo.positions();
        for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
            const auto c = geo.primitivePoints(prim);
            if (c.size() < 3 || !geo.primitiveClosed(prim)) continue;
            Face f;
            Vec3 n, mid;
            for (size_t i = 0; i < c.size(); ++i) {
                const Vec3& a = P[c[i]];
                const Vec3& b = P[c[(i + 1) % c.size()]];
                n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));  // Newell
                mid += a;
                f.corners.push_back(a);
            }
            if (length(n) < 1e-12f) continue;
            f.n = normalize(n);
            f.d = static_cast<double>(dot(f.n, mid * (1.0f / static_cast<float>(c.size()))));
            f.drop = 0;
            for (int a = 1; a < 3; ++a) {
                if (std::fabs(f.n[a]) > std::fabs(f.n[f.drop])) f.drop = a;
            }
            faces_.push_back(std::move(f));
        }
    }

    /// Where the line from `o` along `dir` (of unit length) is inside: the
    /// stretches of it, as distances along it, in order -- where parts of
    /// the solid touch or overlap, one.
    std::vector<std::array<double, 2>> inside(const Vec3& o, const Vec3& dir) const {
        struct Hit {
            double t;
            int way;  // +1 into the solid, -1 out of it
        };
        std::vector<Hit> hits;
        for (const Face& f : faces_) {
            const double along = static_cast<double>(dot(f.n, dir));
            if (std::fabs(along) < 1e-9) continue;
            const double t = (f.d - static_cast<double>(dot(f.n, o))) / along;
            if (!contains(f, o + dir * static_cast<float>(t))) continue;
            hits.push_back({t, along < 0.0 ? 1 : -1});
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.t < b.t || (a.t == b.t && a.way > b.way); });
        // Faces side by side in one plane that both claim a point on the
        // edge between them: one crossing.
        std::vector<Hit> once;
        for (const Hit& h : hits) {
            if (!once.empty() && once.back().way == h.way && std::fabs(once.back().t - h.t) < 1e-7) continue;
            once.push_back(h);
        }
        std::vector<std::array<double, 2>> out;
        int depth = 0;
        double from = 0.0;
        for (const Hit& h : once) {
            const int was = depth;
            depth += h.way;
            if (was <= 0 && depth > 0) from = h.t;
            if (was > 0 && depth <= 0) {
                // Parts that touch: one stretch.
                if (!out.empty() && from - out.back()[1] < 1e-6) out.back()[1] = h.t;
                else out.push_back({from, h.t});
            }
        }
        return out;
    }

private:
    struct Face {
        Vec3 n;
        double d = 0.0;
        int drop = 0;  // the axis the face is looked at along: its normal's biggest
        std::vector<Vec3> corners;
    };

    /// `p`, in the plane of `f`, inside it: an even-odd count of the edges a
    /// line from it crosses, half-open so that of faces side by side a point
    /// on the edge between them is in one.
    static bool contains(const Face& f, const Vec3& p) {
        const int ax = (f.drop + 1) % 3, ay = (f.drop + 2) % 3;
        bool in = false;
        for (size_t i = 0, k = f.corners.size() - 1; i < f.corners.size(); k = i++) {
            const Vec3& a = f.corners[i];
            const Vec3& b = f.corners[k];
            if ((a[ay] > p[ay]) != (b[ay] > p[ay])) {
                const float x = a[ax] + (p[ay] - a[ay]) * (b[ax] - a[ax]) / (b[ay] - a[ay]);
                if (p[ax] < x) in = !in;
            }
        }
        return in;
    }

    std::vector<Face> faces_;
};

enum class Bond { Auto, Stretcher, English, Flemish, Stack };

/// One brick of a course: its stretch along the wall, whether it is a
/// header -- right across the wall -- and whether a joint follows it.
struct Unit {
    double from = 0.0, to = 0.0;
    bool header = false;
    bool joint = false;  ///< the head joint at its end: another brick follows
};

/// What a box of a brick is made of.
enum class Stuff { Brick, Mortar, Plaster };

/// The pieces as they are made: points, and faces with what each is.
struct Builder {
    std::vector<Vec3> points;
    std::vector<std::array<uint32_t, 4>> faces;
    std::vector<Vec3> color;
    std::vector<uint8_t> stuff;  ///< a face's Stuff
    std::vector<int32_t> piece, cluster;
    std::vector<float> clusterGlue;
    std::vector<int32_t> pointPiece;
};

class BrickWallNode : public Node {
public:
    explicit BrickWallNode(std::string name) : Node("brickwall", std::move(name)) {
        setInputCount(1);
        params_.setInt("bond", 0);  // 0 auto, 1 stretcher, 2 English, 3 Flemish, 4 stack
        params_.setFloat("length", 0.25f);
        params_.setFloat("width", 0.12f);
        params_.setFloat("height", 0.065f);
        params_.setFloat("joint", 0.01f);
        params_.setVec3("front", Vec3(0.0f, 0.0f, 1.0f));
        params_.setFloat("plaster", 0.0f);
        params_.setInt("plastersides", 0);  // 0 both, 1 front, 2 back
        params_.setVec3("color", Vec3(0.46f, 0.18f, 0.11f));
        params_.setFloat("variation", 0.35f);
        params_.setVec3("mortar", Vec3(0.52f, 0.5f, 0.47f));
        params_.setVec3("plastercolor", Vec3(0.86f, 0.83f, 0.77f));
        params_.setFloat("broken", 0.3f);
        params_.setFloat("strength", 8.0f);
        params_.setInt("seed", 1);
        params_.setString("attribute", "piece");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0] || in[0]->primitiveCount() == 0) return std::make_shared<Geometry>();
        const Geometry& wall = *in[0];
        // The wall's box: up the way nearest the world's; across the thinner
        // of the others, from the front face in; along the third.
        const OrientedBox box = fitBox(wall, proxyPositions(wall));
        int up = 0;
        for (int a = 1; a < 3; ++a) {
            if (std::fabs(box.axis[static_cast<size_t>(a)].y) > std::fabs(box.axis[static_cast<size_t>(up)].y)) up = a;
        }
        const int o1 = (up + 1) % 3, o2 = (up + 2) % 3;
        const int across = box.size(o1) <= box.size(o2) ? o1 : o2;
        const int along = across == o1 ? o2 : o1;
        Vec3 upDir = box.axis[static_cast<size_t>(up)], acrossDir = box.axis[static_cast<size_t>(across)];
        const Vec3 alongDir = box.axis[static_cast<size_t>(along)];
        const double H = box.size(up), T = box.size(across), Lw = box.size(along);
        std::array<double, 3> corner{};
        corner[static_cast<size_t>(along)] = -0.5 * Lw;
        corner[static_cast<size_t>(up)] = upDir.y < 0.0f ? 0.5 * H : -0.5 * H;
        if (upDir.y < 0.0f) upDir = -upDir;
        const Vec3 front = params_.evalVec3("front", ctx, Vec3(0.0f, 0.0f, 1.0f));
        // w goes in from the front face.
        corner[static_cast<size_t>(across)] = dot(acrossDir, front) >= 0.0f ? 0.5 * T : -0.5 * T;
        if (dot(acrossDir, front) >= 0.0f) acrossDir = -acrossDir;
        const Vec3 origin = box.at(corner);
        const Vec3 inward = acrossDir;
        if (Lw < 1e-6 || H < 1e-6 || T < 1e-6) return std::make_shared<Geometry>();

        const double L = std::max(static_cast<double>(params_.evalFloat("length", ctx, 0.25f)), 0.01);
        const double W = std::max(static_cast<double>(params_.evalFloat("width", ctx, 0.12f)), 0.005);
        const double Hb = std::max(static_cast<double>(params_.evalFloat("height", ctx, 0.065f)), 0.005);
        const double j = std::clamp(static_cast<double>(params_.evalFloat("joint", ctx, 0.01f)), 0.0, 0.1);
        const double plaster = std::clamp(static_cast<double>(params_.evalFloat("plaster", ctx, 0.0f)), 0.0, 0.2);
        const int sides = std::clamp(params_.evalInt("plastersides", ctx, 0), 0, 2);
        const double pf = sides != 2 ? plaster : 0.0, pb = sides != 1 ? plaster : 0.0;
        const Vec3 brick = params_.evalVec3("color", ctx, Vec3(0.46f, 0.18f, 0.11f));
        const double variation = std::clamp(static_cast<double>(params_.evalFloat("variation", ctx, 0.35f)), 0.0, 1.0);
        const Vec3 mortar = params_.evalVec3("mortar", ctx, Vec3(0.52f, 0.5f, 0.47f));
        const Vec3 plasterColor = params_.evalVec3("plastercolor", ctx, Vec3(0.86f, 0.83f, 0.77f));
        const double broken = std::clamp(static_cast<double>(params_.evalFloat("broken", ctx, 0.3f)), 0.0, 1.0);
        const float strength = std::max(params_.evalFloat("strength", ctx, 8.0f), 0.0f);
        const uint64_t seed = static_cast<uint64_t>(std::max(params_.evalInt("seed", ctx, 1), 0)) * 0x9E3779B97F4A7C15ull +
                              0x632BE59BD9B4E019ull;

        // Across: plaster, the leaves of stretchers with a joint between
        // them, plaster.
        const double Tb = T - pf - pb;
        if (Tb < 0.005) return std::make_shared<Geometry>();
        const int leaves = std::max(1, static_cast<int>(std::lround((Tb + j) / (W + j))));
        const double leafPitch = (Tb + j) / leaves;
        Bond bond = static_cast<Bond>(std::clamp(params_.evalInt("bond", ctx, 0), 0, 4));
        if (bond == Bond::Auto) bond = leaves == 1 ? Bond::Stretcher : Bond::English;
        // Up: as many courses as fit, the beds made to fill the height.
        const int courses = std::max(1, static_cast<int>(std::lround(H / (Hb + j))));
        const double pitch = H / courses;
        const double bed = std::clamp(pitch - Hb, 0.0, 0.5 * pitch);
        // Along: a stretcher and its joint, and a header and its.
        const double m = L + j, half = 0.5 * m;
        const double sliver = std::min(0.5 * W, 0.4 * half);
        // A brick breaks in two when it is half as long again as it is wide.
        const double breakable = std::min(1.5 * W, 0.8 * L);

        const Solid solid(wall);
        Builder b;
        int32_t count = 0, clusters = 0;
        for (int c = 0; c < courses && !ctx.interrupted(); ++c) {
            const double h0 = c * pitch, h1 = (c + 1) * pitch;
            // The stretches of the course inside the wall, through its middle.
            const Vec3 o = origin + upDir * static_cast<float>(0.5 * (h0 + h1)) + inward * static_cast<float>(0.5 * T);
            const std::vector<std::array<double, 2>> stretches = solid.inside(o, alongDir);
            for (size_t si = 0; si < stretches.size(); ++si) {
                const double a = std::max(stretches[si][0], 0.0), z = std::min(stretches[si][1], Lw);
                if (z - a < sliver) continue;
                const std::vector<Unit> laid = units(bond, c, a, z, m, half, sliver);
                for (size_t ui = 0; ui < laid.size(); ++ui) {
                    const Unit& u = laid[ui];
                    const double s1 = u.joint ? u.to - j : u.to;
                    // Across: a header right through, else a brick a leaf --
                    // the first with the plaster in front, the last with the
                    // plaster behind, the others with a joint behind.
                    const int n = u.header ? 1 : leaves;
                    for (int k = 0; k < n; ++k) {
                        const bool first = k == 0, last = k + 1 == n;
                        const double leaf = pf + k * leafPitch;
                        const double w0 = first ? 0.0 : leaf;
                        const double wf = first ? pf : leaf;
                        const double wb = last ? T - pb : leaf + leafPitch - j;
                        const double w1 = last ? T : leaf + leafPitch;
                        // Its own draws -- the same whatever the bricks
                        // round it do -- and its colour, a shade of its own.
                        uint64_t state = mix(mix(mix(mix(seed, c), static_cast<int>(si)), static_cast<int>(ui)), k);
                        const float shade = static_cast<float>(1.0 + variation * (unit(state) - 0.5));
                        const float warm = static_cast<float>(1.0 + 0.6 * variation * (unit(state) - 0.5));
                        const Vec3 face(std::min(brick.x * shade * warm, 1.0f), std::min(brick.y * shade, 1.0f),
                                        std::min(brick.z * shade / warm, 1.0f));
                        const Vec3 inner(std::min(face.x * 1.12f + 0.04f, 1.0f), std::min(face.y * 1.12f + 0.02f, 1.0f),
                                         std::min(face.z * 1.1f, 1.0f));
                        // Broken ones: two halves, one cluster.
                        const double roll = unit(state), cut = unit(state);
                        const bool breaks = broken > 0.0 && s1 - u.from > breakable && roll < broken;
                        std::vector<std::array<double, 2>> parts;
                        if (breaks) {
                            const double at = u.from + (s1 - u.from) * (0.35 + 0.3 * cut);
                            parts = {{u.from, at}, {at, u.to}};
                            ++clusters;
                        } else {
                            parts = {{u.from, u.to}};
                        }
                        for (size_t q = 0; q < parts.size(); ++q) {
                            Cell cell;
                            cell.s = {parts[q][0], std::min(s1, parts[q][1]), parts[q][1]};
                            cell.h = {h0, h0 + bed, h1};
                            cell.w0 = w0;
                            cell.wf = wf;
                            cell.wb = wb;
                            cell.w1 = w1;
                            cell.backPlaster = last;
                            cell.cutLow = q == 1;
                            cell.cutHigh = breaks && q == 0;
                            cell.brick = face;
                            cell.inner = inner;
                            addCell(b, cell, origin, alongDir, upDir, inward, mortar, plasterColor, count);
                            b.cluster.resize(b.color.size(), breaks ? clusters : 0);
                            b.clusterGlue.resize(b.color.size(), breaks ? strength : 1.0f);
                            ++count;
                        }
                    }
                }
            }
        }
        if (ctx.interrupted()) return nullptr;

        auto out = std::make_shared<Geometry>();
        out->addPoints(b.points.size());
        auto P = out->positionsForWrite();
        std::copy(b.points.begin(), b.points.end(), P.begin());
        for (const auto& f : b.faces) out->addPrimitive(f, true);
        const std::string attribute = params_.getString("attribute", "piece");
        auto cd = out->primitives().create("Cd", AttrType::Vec3).write<Vec3>();
        std::copy(b.color.begin(), b.color.end(), cd.begin());
        // What each face is, for a renderer (s@material).
        for (const auto& [what, name] : {std::pair{Stuff::Brick, "brick"}, std::pair{Stuff::Mortar, "mortar"},
                                         std::pair{Stuff::Plaster, "plaster"}}) {
            std::vector<uint8_t> mask(b.stuff.size());
            for (size_t i = 0; i < mask.size(); ++i) mask[i] = b.stuff[i] == static_cast<uint8_t>(what);
            setPrimitiveString(*out, "material", name, mask);
        }
        if (!attribute.empty()) {
            auto pp = out->primitives().create(attribute, AttrType::Int).write<int32_t>();
            std::copy(b.piece.begin(), b.piece.end(), pp.begin());
            auto pt = out->points().create(attribute, AttrType::Int).write<int32_t>();
            std::copy(b.pointPiece.begin(), b.pointPiece.end(), pt.begin());
        }
        if (clusters > 0) {
            auto cl = out->primitives().create("cluster", AttrType::Int).write<int32_t>();
            std::copy(b.cluster.begin(), b.cluster.end(), cl.begin());
            auto cg = out->primitives().create("clusterglue", AttrType::Float).write<float>();
            std::copy(b.clusterGlue.begin(), b.clusterGlue.end(), cg.begin());
        }
        return out;
    }

private:
    /// A brick with its mortar and plaster, in the wall's own measures: along
    /// s -- the brick to s[1], its head joint to s[2] -- up h -- its bed to
    /// h[1] -- and in from the front w: plaster to wf, the brick to wb, a
    /// joint or plaster to w1.
    struct Cell {
        std::array<double, 3> s{}, h{};
        double w0 = 0.0, wf = 0.0, wb = 0.0, w1 = 0.0;
        bool backPlaster = false;              // behind the brick plaster, not a joint
        bool cutLow = false, cutHigh = false;  // a break at its start, or its end, along
        Vec3 brick, inner;
    };

    /// The bricks of course `c` along [a, z]: the units of the bond, cut
    /// short at the ends, a sliver given to the unit beside it.
    static std::vector<Unit> units(Bond bond, int c, double a, double z, double m, double half, double sliver) {
        // The pattern: where it starts, and the units it repeats.
        double offset = 0.0;
        std::vector<std::pair<double, bool>> pattern;  // length, header
        switch (bond) {
            case Bond::Stack: pattern = {{m, false}}; break;
            case Bond::English:
                if (c % 2 == 0) {
                    pattern = {{half, true}};
                } else {
                    pattern = {{m, false}};
                    offset = 0.25 * m;
                }
                break;
            case Bond::Flemish:
                pattern = {{half, true}, {m, false}};
                offset = c % 2 == 0 ? 0.0 : 0.75 * m;
                break;
            default:
                pattern = {{m, false}};
                offset = c % 2 == 0 ? 0.0 : 0.5 * m;
                break;
        }
        double period = 0.0;
        for (const auto& p : pattern) period += p.first;
        // The first whole period at or before a.
        double s = -offset + std::floor((a + offset) / period) * period;
        size_t k = 0;
        std::vector<Unit> out;
        while (s < z - 1e-9) {
            const auto& p = pattern[k];
            const double e = s + p.first;
            if (e > a + 1e-9) out.push_back({std::max(s, a), std::min(e, z), p.second, e < z - 1e-9});
            s = e;
            k = (k + 1) % pattern.size();
        }
        // Slivers: at the ends, cut short, given to the unit beside.
        if (out.size() > 1 && out.front().to - out.front().from < sliver) {
            out[1].from = out.front().from;
            out.erase(out.begin());
        }
        if (out.size() > 1 && out.back().to - out.back().from < sliver) {
            out[out.size() - 2].to = out.back().to;
            out[out.size() - 2].joint = false;
            out.pop_back();
        }
        return out;
    }

    /// The cell as a closed box, each face split where brick, mortar and
    /// plaster meet, each piece of face what is behind it.
    static void addCell(Builder& b, const Cell& c, const Vec3& origin, const Vec3& along, const Vec3& up, const Vec3& in,
                        const Vec3& mortar, const Vec3& plaster, int32_t piece) {
        auto cuts = [](std::initializer_list<double> v) {
            std::vector<double> out(v);
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end(), [](double x, double y) { return std::fabs(x - y) < 1e-9; }), out.end());
            return out;
        };
        const std::vector<double> S = cuts({c.s[0], c.s[1], c.s[2]}), Hh = cuts({c.h[0], c.h[1], c.h[2]}),
                                  Ww = cuts({c.w0, c.wf, c.wb, c.w1});
        const size_t ns = S.size(), nh = Hh.size(), nw = Ww.size();
        if (ns < 2 || nh < 2 || nw < 2) return;
        // The points on its surface.
        std::vector<uint32_t> id(ns * nh * nw, UINT32_MAX);
        auto point = [&](size_t i, size_t k, size_t l) {
            uint32_t& at = id[(i * nh + k) * nw + l];
            if (at == UINT32_MAX) {
                at = static_cast<uint32_t>(b.points.size());
                b.points.push_back(origin + along * static_cast<float>(S[i]) + up * static_cast<float>(Hh[k]) +
                                   in * static_cast<float>(Ww[l]));
                b.pointPiece.push_back(piece);
            }
            return at;
        };
        // What the box between cuts i, k, l is made of, and its colour on a face.
        auto stuff = [&](size_t i, size_t k, size_t l) {
            const double s = 0.5 * (S[i] + S[i + 1]), h = 0.5 * (Hh[k] + Hh[k + 1]), w = 0.5 * (Ww[l] + Ww[l + 1]);
            if (w < c.wf) return Stuff::Plaster;
            if (w > c.wb) return c.backPlaster ? Stuff::Plaster : Stuff::Mortar;
            if (h < c.h[1] || s > c.s[1]) return Stuff::Mortar;
            return Stuff::Brick;
        };
        auto colour = [&](Stuff what, bool broken) {
            if (what == Stuff::Plaster) return plaster;
            if (what == Stuff::Mortar) return mortar;
            return broken ? c.inner : c.brick;
        };
        // Handedness: the corners go round the faces so that they face out.
        const bool flip = dot(cross(along, up), in) < 0.0f;
        auto face = [&](std::array<uint32_t, 4> q, Stuff what, bool broken) {
            if (flip) std::swap(q[1], q[3]);
            b.faces.push_back(q);
            b.color.push_back(colour(what, broken));
            b.stuff.push_back(static_cast<uint8_t>(what));
            b.piece.push_back(piece);
        };
        // Along: the faces at its start and end.
        for (size_t k = 0; k + 1 < nh; ++k) {
            for (size_t l = 0; l + 1 < nw; ++l) {
                face({point(0, k, l), point(0, k, l + 1), point(0, k + 1, l + 1), point(0, k + 1, l)}, stuff(0, k, l), c.cutLow);
                const size_t e = ns - 1;
                face({point(e, k, l), point(e, k + 1, l), point(e, k + 1, l + 1), point(e, k, l + 1)}, stuff(e - 1, k, l),
                     c.cutHigh);
            }
        }
        // Up: its bottom and top.
        for (size_t i = 0; i + 1 < ns; ++i) {
            for (size_t l = 0; l + 1 < nw; ++l) {
                face({point(i, 0, l), point(i + 1, 0, l), point(i + 1, 0, l + 1), point(i, 0, l + 1)}, stuff(i, 0, l), false);
                const size_t e = nh - 1;
                face({point(i, e, l), point(i, e, l + 1), point(i + 1, e, l + 1), point(i + 1, e, l)}, stuff(i, e - 1, l),
                     false);
            }
        }
        // In: its front and back.
        for (size_t i = 0; i + 1 < ns; ++i) {
            for (size_t k = 0; k + 1 < nh; ++k) {
                face({point(i, k, 0), point(i, k + 1, 0), point(i + 1, k + 1, 0), point(i + 1, k, 0)}, stuff(i, k, 0), false);
                const size_t e = nw - 1;
                face({point(i, k, e), point(i + 1, k, e), point(i + 1, k + 1, e), point(i, k + 1, e)}, stuff(i, k, e - 1),
                     false);
            }
        }
    }
};

}  // namespace

void registerBrickNodes() {
    NodeRegistry::instance().add("brickwall", [](const std::string& n) { return std::make_unique<BrickWallNode>(n); });
}

}  // namespace pg
