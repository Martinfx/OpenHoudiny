#include "pg/core/Sculpt.h"

#include "pg/core/Parallel.h"
#include "pg/core/Spatial.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace pg {

/// Who each point is smoothed towards: its neighbours along the edges --
/// or, on an open border, its two neighbours along the border, where the
/// border runs on straight through it; a corner stays.
struct SculptSmoothing {
    Adjacency adjacency;
    std::vector<uint8_t> borders;                ///< how many border edges each point is on
    std::vector<std::array<uint32_t, 2>> along;  ///< ... the other ends of the first two
    std::vector<uint8_t> straight;               ///< the border runs straight on through it, as it was made

    /// Of `geo` before any dab: what was a corner stays one.
    explicit SculptSmoothing(const Geometry& geo) {
        adjacency.build(geo);
        const size_t n = geo.pointCount();
        borders.assign(n, 0);
        along.assign(n, {0, 0});
        // A side of one closed polygon only is a border. Each side is
        // counted at its place among the neighbours of its lower end.
        std::vector<size_t> at(n + 1, 0);
        for (size_t i = 0; i < n; ++i) at[i + 1] = at[i] + adjacency.neighbours(i).size();
        std::vector<uint8_t> faces(at[n], 0);
        for (size_t p = 0; p < geo.primitiveCount(); ++p) {
            const auto pts = geo.primitivePoints(p);
            if (pts.size() < 3 || !geo.primitiveClosed(p)) continue;
            for (size_t k = 0; k < pts.size(); ++k) {
                const uint32_t a = std::min(pts[k], pts[(k + 1) % pts.size()]);
                const uint32_t b = std::max(pts[k], pts[(k + 1) % pts.size()]);
                if (a == b || b >= n) continue;
                const auto nb = adjacency.neighbours(a);
                const auto it = std::lower_bound(nb.begin(), nb.end(), static_cast<int32_t>(b));
                if (it == nb.end() || *it != static_cast<int32_t>(b)) continue;
                uint8_t& count = faces[at[a] + static_cast<size_t>(it - nb.begin())];
                if (count < 2) ++count;
            }
        }
        for (size_t a = 0; a < n; ++a) {
            const auto nb = adjacency.neighbours(a);
            for (size_t k = 0; k < nb.size(); ++k) {
                if (faces[at[a] + k] != 1) continue;
                const uint32_t b = static_cast<uint32_t>(nb[k]);
                if (borders[a] < 2) along[a][borders[a]] = b;
                if (borders[b] < 2) along[b][borders[b]] = static_cast<uint32_t>(a);
                borders[a] = static_cast<uint8_t>(std::min(borders[a] + 1, 255));
                borders[b] = static_cast<uint8_t>(std::min(borders[b] + 1, 255));
            }
        }
        // Straight on within 30 degrees: along the border; else a corner.
        const auto P = geo.positions();
        straight.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            if (borders[i] != 2) continue;
            const Vec3 a = P[along[i][0]] - P[i], b = P[along[i][1]] - P[i];
            const float la = length(a), lb = length(b);
            straight[i] = la > 1e-12f && lb > 1e-12f && dot(a, b) <= -0.866f * la * lb ? 1 : 0;
        }
    }

    /// Where point `i` is smoothed towards; false where it stays -- a
    /// corner of a border, where borders meet, the end of a line.
    bool target(std::span<const Vec3> P, uint32_t i, Vec3& out) const {
        if (borders[i] > 0) {
            if (!straight[i]) return false;
            out = (P[along[i][0]] + P[along[i][1]]) * 0.5f;
            return true;
        }
        const auto nb = adjacency.neighbours(i);
        if (nb.size() < 2) return false;
        Vec3 sum;
        for (const int32_t j : nb) sum += P[static_cast<size_t>(j)];
        out = sum * (1.0f / static_cast<float>(nb.size()));
        return true;
    }
};

namespace {

/// The points in the cells of a grid, moved from cell to cell as they
/// move: who is near a place, while the places change.
class MovingGrid {
public:
    MovingGrid(std::span<const Vec3> P, float cell) : inv_(1.0 / static_cast<double>(cell)) {
        cellOf_.resize(P.size());
        slot_.resize(P.size());
        for (size_t i = 0; i < P.size(); ++i) insert(static_cast<uint32_t>(i), key(P[i]));
    }

    void moved(uint32_t i, const Vec3& p) {
        const uint64_t k = key(p);
        if (k == cellOf_[i]) return;
        remove(i);
        insert(i, k);
    }

    /// The points nearer `c` than `r`, each once, in no set order.
    void near(std::span<const Vec3> P, const Vec3& c, float r, std::vector<uint32_t>& out) const {
        out.clear();
        const float r2 = r * r;
        int64_t lo[3], hi[3];
        double cells = 1.0;
        bool shared = false;
        for (int k = 0; k < 3; ++k) {
            // A little further than the radius: no point the test below
            // lets in is missed by how the cell's number rounds.
            const float reach = r + 1e-5f * (std::fabs(c[k]) + r);
            lo[k] = index(c[k] - reach);
            hi[k] = index(c[k] + reach);
            cells *= static_cast<double>(hi[k] - lo[k] + 1);
            shared = shared || hi[k] - lo[k] + 1 >= (int64_t(1) << 21);
        }
        if (cells > 4.0 * static_cast<double>(P.size()) + 64.0) {
            // More cells than points: every point, once.
            for (size_t i = 0; i < P.size(); ++i) {
                const Vec3 d = P[i] - c;
                if (dot(d, d) < r2) out.push_back(static_cast<uint32_t>(i));
            }
            return;
        }
        for (int64_t x = lo[0]; x <= hi[0]; ++x) {
            for (int64_t y = lo[1]; y <= hi[1]; ++y) {
                for (int64_t z = lo[2]; z <= hi[2]; ++z) {
                    const auto it = cells_.find(pack(x, y, z));
                    if (it == cells_.end()) continue;
                    for (const uint32_t i : it->second) {
                        const Vec3 d = P[i] - c;
                        if (dot(d, d) < r2) out.push_back(i);
                    }
                }
            }
        }
        if (shared) {
            // Cells 2^21 apart share a key, and were both asked: each point once.
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }
    }

private:
    int64_t index(float v) const {
        const double x = std::floor(static_cast<double>(v) * inv_);
        return std::isfinite(x) ? static_cast<int64_t>(std::clamp(x, -1e15, 1e15)) : 0;
    }
    static uint64_t pack(int64_t x, int64_t y, int64_t z) {
        constexpr uint64_t m = (1ull << 21) - 1;
        return (static_cast<uint64_t>(x) & m) | ((static_cast<uint64_t>(y) & m) << 21) | ((static_cast<uint64_t>(z) & m) << 42);
    }
    uint64_t key(const Vec3& p) const { return pack(index(p.x), index(p.y), index(p.z)); }
    void insert(uint32_t i, uint64_t k) {
        std::vector<uint32_t>& v = cells_[k];
        slot_[i] = static_cast<uint32_t>(v.size());
        v.push_back(i);
        cellOf_[i] = k;
    }
    void remove(uint32_t i) {
        std::vector<uint32_t>& v = cells_[cellOf_[i]];
        const uint32_t last = v.back();
        v[slot_[i]] = last;
        slot_[last] = slot_[i];
        v.pop_back();
    }

    double inv_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
    std::vector<uint64_t> cellOf_;
    std::vector<uint32_t> slot_;
};

/// Up to this many dabs, every point is asked where each dab is -- quicker
/// than making a grid first, as when a stroke adds a dab or two.
constexpr size_t kScanDabs = 32;

/// The points nearer `c` than `r`, every point asked; in number order.
void scan(std::span<const Vec3> P, const Vec3& c, float r, std::vector<uint32_t>& out) {
    out.clear();
    const float r2 = r * r;
    const auto chunks = chunkRanges(P.size(), size_t(1) << 15);
    if (chunks.size() <= 1) {
        for (size_t i = 0; i < P.size(); ++i) {
            const Vec3 d = P[i] - c;
            if (dot(d, d) < r2) out.push_back(static_cast<uint32_t>(i));
        }
        return;
    }
    std::vector<std::vector<uint32_t>> found(chunks.size());
    TaskPool::instance().run(chunks.size(), [&](size_t k) {
        for (size_t i = chunks[k].first; i < chunks[k].second; ++i) {
            const Vec3 d = P[i] - c;
            if (dot(d, d) < r2) found[k].push_back(static_cast<uint32_t>(i));
        }
    });
    for (const auto& f : found) out.insert(out.end(), f.begin(), f.end());
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool sameDab(const SculptDab& a, const SculptDab& b) {
    return a.tool == b.tool && a.at == b.at && a.normal == b.normal && a.move == b.move && a.radius == b.radius &&
           a.strength == b.strength;
}

bool smooths(std::span<const SculptDab> dabs) {
    return std::any_of(dabs.begin(), dabs.end(), [](const SculptDab& d) { return d.tool == SculptDab::Tool::Smooth; });
}

/// The dabs on the points of `geo`, one after another; true if a point moved.
bool apply(Geometry& geo, std::span<const SculptDab> dabs, Falloff shape, const SculptSmoothing* smoothing) {
    const size_t n = geo.pointCount();
    if (n == 0 || dabs.empty()) return false;
    auto P = geo.positionsForWrite();
    // Many dabs: a grid, its cells as big as the dabs mostly are.
    std::unique_ptr<MovingGrid> grid;
    if (dabs.size() > kScanDabs) {
        std::vector<float> radii;
        radii.reserve(dabs.size());
        for (const SculptDab& d : dabs) radii.push_back(d.radius);
        std::nth_element(radii.begin(), radii.begin() + static_cast<std::ptrdiff_t>(radii.size() / 2), radii.end());
        grid = std::make_unique<MovingGrid>(P, std::max(radii[radii.size() / 2], 1e-6f));
    }
    std::vector<uint32_t> near;
    std::vector<Vec3> next;
    bool moved = false;
    for (const SculptDab& d : dabs) {
        if (!(d.radius > 0.0f) || !finite(d.at)) continue;
        if (grid) grid->near(P, d.at, d.radius, near);
        else scan(P, d.at, d.radius, near);
        if (near.empty()) continue;
        // Each point's share: as far from the middle as it was before this
        // dab; each moved as the points were before it, so in any order.
        next.resize(near.size());
        const auto step = [&](size_t begin, size_t end) {
            for (size_t k = begin; k < end; ++k) {
                const uint32_t i = near[k];
                const float w = falloff(shape, length(P[i] - d.at) / d.radius);
                Vec3 p = P[i];
                switch (d.tool) {
                    case SculptDab::Tool::Push: p += d.normal * (d.strength * kSculptPush * d.radius * w); break;
                    case SculptDab::Tool::Grab: p += d.move * w; break;
                    case SculptDab::Tool::Flatten: p += d.normal * (-dot(P[i] - d.at, d.normal) * d.strength * w); break;
                    case SculptDab::Tool::Smooth: {
                        Vec3 to;
                        if (smoothing && smoothing->target(P, i, to)) p += (to - P[i]) * (d.strength * w);
                        break;
                    }
                }
                next[k] = finite(p) ? p : P[i];
            }
        };
        if (near.size() >= 8192) parallelFor(near.size(), 2048, step);
        else step(0, near.size());
        for (size_t k = 0; k < near.size(); ++k) {
            const uint32_t i = near[k];
            if (next[k] == P[i]) continue;
            P[i] = next[k];
            if (grid) grid->moved(i, P[i]);
            moved = true;
        }
    }
    return moved;
}

/// The point normals N, where there are, found again from the faces round
/// each point.
void renormal(Geometry& geo) {
    AttributeArray* nAttr = geo.points().find("N");
    if (!nAttr || nAttr->type() != AttrType::Vec3) return;
    const size_t n = geo.pointCount();
    std::vector<Vec3> sum(n);
    const auto Q = geo.positions();
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        const auto pts = geo.primitivePoints(p);
        if (pts.size() < 3 || !geo.primitiveClosed(p)) continue;
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            if (pts[0] >= n || pts[k] >= n || pts[k + 1] >= n) continue;
            const Vec3 c = cross(Q[pts[k]] - Q[pts[0]], Q[pts[k + 1]] - Q[pts[0]]);
            for (const uint32_t q : {pts[0], pts[k], pts[k + 1]}) sum[q] += c;
        }
    }
    auto N = nAttr->write<Vec3>();
    for (size_t i = 0; i < n; ++i) {
        const float l = length(sum[i]);
        if (l > 1e-20f) N[i] = sum[i] * (1.0f / l);
    }
}

}  // namespace

// --- as text ------------------------------------------------------------------------------

std::vector<SculptDab> parseSculpt(std::string_view text) {
    std::vector<SculptDab> out;
    const std::string s(text);
    const char* c = s.c_str();
    while (*c) {
        while (*c == ' ' || *c == ';' || *c == '\n' || *c == '\t') ++c;
        if (!*c) break;
        const char tool = *c++;
        const int want = tool == 'p' || tool == 'f' ? 8 : tool == 's' ? 5 : tool == 'g' ? 7 : 0;
        float v[8] = {};
        int k = 0;
        for (; k < want; ++k) {
            char* end = nullptr;
            v[k] = std::strtof(c, &end);
            if (end == c) break;
            c = end;
        }
        bool ok = want > 0 && k == want;
        for (int i = 0; i < k; ++i) ok = ok && std::isfinite(v[i]);
        if (ok) {
            SculptDab d;
            d.at = Vec3(v[0], v[1], v[2]);
            switch (tool) {
                case 'p':
                case 'f':
                    d.tool = tool == 'p' ? SculptDab::Tool::Push : SculptDab::Tool::Flatten;
                    d.normal = normalize(Vec3(v[3], v[4], v[5]));
                    d.radius = v[6];
                    d.strength = v[7];
                    ok = length(d.normal) > 0.5f;
                    break;
                case 's':
                    d.tool = SculptDab::Tool::Smooth;
                    d.radius = v[3];
                    d.strength = v[4];
                    break;
                default:
                    d.tool = SculptDab::Tool::Grab;
                    d.move = Vec3(v[3], v[4], v[5]);
                    d.radius = v[6];
                    d.strength = 1.0f;
                    break;
            }
            d.strength = d.tool == SculptDab::Tool::Push ? std::clamp(d.strength, -4.0f, 4.0f) : std::clamp(d.strength, 0.0f, 1.0f);
            if (ok && d.radius > 0.0f) out.push_back(d);
        }
        // On to the next dab.
        while (*c && *c != ';') ++c;
    }
    return out;
}

std::string sculptText(const SculptDab& d) {
    char text[200];
    const auto f = [](float x) { return static_cast<double>(x); };
    switch (d.tool) {
        case SculptDab::Tool::Push:
        case SculptDab::Tool::Flatten:
            std::snprintf(text, sizeof text, "%c %.6g %.6g %.6g %.4g %.4g %.4g %.4g %.4g",
                          d.tool == SculptDab::Tool::Push ? 'p' : 'f', f(d.at.x), f(d.at.y), f(d.at.z), f(d.normal.x),
                          f(d.normal.y), f(d.normal.z), f(d.radius), f(d.strength));
            break;
        case SculptDab::Tool::Smooth:
            std::snprintf(text, sizeof text, "s %.6g %.6g %.6g %.4g %.4g", f(d.at.x), f(d.at.y), f(d.at.z), f(d.radius),
                          f(d.strength));
            break;
        case SculptDab::Tool::Grab:
            std::snprintf(text, sizeof text, "g %.6g %.6g %.6g %.5g %.5g %.5g %.4g", f(d.at.x), f(d.at.y), f(d.at.z),
                          f(d.move.x), f(d.move.y), f(d.move.z), f(d.radius));
            break;
    }
    return text;
}

// --- the dabs on a geometry --------------------------------------------------------------

void sculpt(Geometry& geo, std::span<const SculptDab> dabs, Falloff shape) {
    if (geo.pointCount() == 0 || dabs.empty()) return;
    // Who is smoothed towards whom, of the geometry as it came.
    std::unique_ptr<SculptSmoothing> smoothing;
    if (smooths(dabs)) smoothing = std::make_unique<SculptSmoothing>(geo);
    if (apply(geo, dabs, shape, smoothing.get())) renormal(geo);
}

// --- going on from where it got to ------------------------------------------------------

GeometryPtr Sculptor::cook(const GeometryPtr& source, std::span<const SculptDab> dabs, Falloff shape) {
    reused_ = 0;
    if (!source || source->pointCount() == 0 || dabs.empty()) return editableCopy(source);
    if (source != source_ || shape != shape_) {
        if (source != source_) smoothing_.reset();
        source_ = source;
        shape_ = shape;
        dabs_.clear();
        before_.reset();
        after_.reset();
        movedBefore_ = movedAfter_ = false;
    }
    // How many of the dabs, from the first on, it had.
    size_t same = 0;
    while (same < dabs.size() && same < dabs_.size() && sameDab(dabs[same], dabs_[same])) ++same;
    std::shared_ptr<Geometry> geo;
    size_t from = 0;
    bool moved = false;
    if (after_ && same == dabs_.size()) {
        if (same == dabs.size()) {
            reused_ = same;
            return after_;  // the same dabs: what it gave
        }
        geo = editableCopy(after_);  // more after them
        from = same;
        moved = movedAfter_;
    } else if (before_ && same + 1 == dabs_.size()) {
        geo = editableCopy(before_);  // the last one other, or gone
        from = same;
        moved = movedBefore_;
    } else {
        geo = editableCopy(source);
    }
    reused_ = from;
    if (!smoothing_ && smooths(dabs.subspan(from))) smoothing_ = std::make_shared<const SculptSmoothing>(*source);

    // All the new dabs but the last; the geometry then kept, for the next
    // cook to go on from if only the last one changes.
    const size_t m = dabs.size();
    GeometryPtr before;
    bool movedBefore = false;
    if (from < m) {
        moved = apply(*geo, dabs.subspan(from, m - 1 - from), shape, smoothing_.get()) || moved;
        before = std::make_shared<const Geometry>(*geo);
        movedBefore = moved;
        moved = apply(*geo, dabs.subspan(m - 1), shape, smoothing_.get()) || moved;
    }
    // The normals of the positions as they are, as `sculpt` finds them.
    if (moved) renormal(*geo);
    before_ = std::move(before);
    movedBefore_ = movedBefore;
    after_ = geo;
    movedAfter_ = moved;
    dabs_.assign(dabs.begin(), dabs.end());
    return after_;
}

}  // namespace pg
