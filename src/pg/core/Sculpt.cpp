#include "pg/core/Sculpt.h"

#include "pg/core/Parallel.h"
#include "pg/core/Spatial.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

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

/// Up to this many dabs, every point is asked where each dab is -- quicker
/// than making a grid first, as when a stroke adds a dab or two.
constexpr size_t kScanDabs = 32;

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool sameDab(const SculptDab& a, const SculptDab& b) {
    return a.tool == b.tool && a.at == b.at && a.normal == b.normal && a.move == b.move && a.radius == b.radius &&
           a.strength == b.strength && a.joined == b.joined;
}

/// Where the last group of `dabs` begins: its dab that is not joined to
/// the one before.
size_t lastGroup(std::span<const SculptDab> dabs) {
    size_t k = dabs.size();
    while (k > 1 && dabs[k - 1].joined) --k;
    return k == 0 ? 0 : k - 1;
}

bool smooths(std::span<const SculptDab> dabs) {
    return std::any_of(dabs.begin(), dabs.end(), [](const SculptDab& d) { return d.tool == SculptDab::Tool::Smooth; });
}

/// The radius most of the dabs have: the size of a grid's cells.
float typicalRadius(std::span<const SculptDab> dabs) {
    std::vector<float> radii;
    radii.reserve(dabs.size());
    for (const SculptDab& d : dabs) radii.push_back(d.radius);
    std::nth_element(radii.begin(), radii.begin() + static_cast<std::ptrdiff_t>(radii.size() / 2), radii.end());
    return std::max(radii[radii.size() / 2], 1e-6f);
}

/// The points of a geometry, as they are: what the dabs move without
/// dyntopo.
class FixedSurface {
public:
    FixedSurface(Geometry& geo, std::span<const SculptDab> dabs, const SculptSmoothing* smoothing)
        : P_(geo.positionsForWrite()), smoothing_(smoothing) {
        // Many dabs: a grid, its cells as big as the dabs mostly are.
        if (dabs.size() > kScanDabs) grid_ = std::make_unique<MovingGrid>(P_, typicalRadius(dabs));
    }
    std::span<Vec3> positions() { return P_; }
    void refine(const SculptDab&) {}
    void find(const SculptDab& d, std::vector<uint32_t>& near) const {
        if (grid_) grid_->near(P_, d.at, d.radius, near);
        else scanNear(P_, d.at, d.radius, near);
    }
    bool target(std::span<const Vec3> P, uint32_t i, Vec3& to) const { return smoothing_ && smoothing_->target(P, i, to); }
    void moved(uint32_t i) {
        if (grid_) grid_->moved(i, P_[i]);
    }

private:
    std::span<Vec3> P_;
    const SculptSmoothing* smoothing_;
    std::unique_ptr<MovingGrid> grid_;
};

/// A mesh made finer and coarser under each dab as it comes (Dyntopo.h).
class DynamicSurface {
public:
    DynamicSurface(SculptMesh& mesh, std::span<const SculptDab> dabs, const Dyntopo& dyntopo)
        : mesh_(mesh), dyntopo_(dyntopo) {
        if (dabs.size() > kScanDabs) mesh_.index(typicalRadius(dabs));
    }
    ~DynamicSurface() { mesh_.unindex(); }
    DynamicSurface(const DynamicSurface&) = delete;
    DynamicSurface& operator=(const DynamicSurface&) = delete;

    std::span<Vec3> positions() { return mesh_.positions(); }
    /// Grab takes what it holds as it is, as in Blender.
    void refine(const SculptDab& d) {
        if (d.tool != SculptDab::Tool::Grab) mesh_.refine(d.at, d.radius, dyntopo_);
    }
    void find(const SculptDab& d, std::vector<uint32_t>& near) const { mesh_.near(d.at, d.radius, near); }
    bool target(std::span<const Vec3> P, uint32_t i, Vec3& to) const { return mesh_.target(P, i, to); }
    void moved(uint32_t i) { mesh_.moved(i); }

private:
    SculptMesh& mesh_;
    Dyntopo dyntopo_;
};

/// The dabs on the points of a surface, one after another; true if a
/// point moved. Each group of joined dabs first makes the mesh as fine as
/// it says (with dyntopo), then moves its points.
template <class Surface>
bool apply(Surface& surface, std::span<const SculptDab> dabs, Falloff shape) {
    if (dabs.empty() || surface.positions().empty()) return false;
    std::span<Vec3> P;
    std::vector<uint32_t> near;
    std::vector<Vec3> next;
    std::vector<std::pair<uint32_t, Vec3>> moves;  // a group's: each point and how far a dab moves it
    bool moved = false;
    // Where dab `d` takes the points `near` holds: each by its share, as far
    // from the middle as it was before the dab -- each moved as the points
    // were before it, so in any order.
    const auto takes = [&](const SculptDab& d) {
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
                        if (surface.target(P, i, to)) p += (to - P[i]) * (d.strength * w);
                        break;
                    }
                }
                next[k] = finite(p) ? p : P[i];
            }
        };
        if (near.size() >= 8192) parallelFor(near.size(), 2048, step);
        else step(0, near.size());
    };
    const auto valid = [](const SculptDab& d) { return d.radius > 0.0f && finite(d.at); };
    for (size_t g = 0; g < dabs.size();) {
        size_t end = g + 1;
        while (end < dabs.size() && dabs[end].joined) ++end;
        for (size_t k = g; k < end; ++k) {
            if (valid(dabs[k])) surface.refine(dabs[k]);
        }
        P = surface.positions();
        if (end == g + 1) {
            const SculptDab& d = dabs[g];
            g = end;
            if (!valid(d)) continue;
            surface.find(d, near);
            if (near.empty()) continue;
            takes(d);
            for (size_t k = 0; k < near.size(); ++k) {
                const uint32_t i = near[k];
                if (next[k] == P[i]) continue;
                P[i] = next[k];
                surface.moved(i);
                moved = true;
            }
            continue;
        }
        // Dabs joined -- a dab and its mirror image: each point moved by
        // all of them, as the points were before them; summed in the
        // order of the dabs.
        moves.clear();
        for (size_t k = g; k < end; ++k) {
            const SculptDab& d = dabs[k];
            if (!valid(d)) continue;
            surface.find(d, near);
            if (near.empty()) continue;
            takes(d);
            for (size_t j = 0; j < near.size(); ++j) moves.push_back({near[j], next[j] - P[near[j]]});
        }
        g = end;
        std::stable_sort(moves.begin(), moves.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t k = 0; k < moves.size();) {
            const uint32_t i = moves[k].first;
            Vec3 by(0.0f);
            for (; k < moves.size() && moves[k].first == i; ++k) by += moves[k].second;
            const Vec3 p = P[i] + by;
            if (p == P[i] || !finite(p)) continue;
            P[i] = p;
            surface.moved(i);
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
        const bool joined = *c == '+';
        if (joined) ++c;
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
            d.joined = joined && !out.empty();
            if (ok && d.radius > 0.0f) out.push_back(d);
        }
        // On to the next dab.
        while (*c && *c != ';') ++c;
    }
    return out;
}

std::array<SculptDab, 2> mirroredDabs(const SculptDab& d, Mirror m) {
    SculptDab a = d, b = d;
    b.at = mirrored(d.at, m);
    b.normal = mirrored(d.normal, m);
    b.move = mirrored(d.move, m);
    b.joined = true;
    // Where the two overlap, each weaker: half where they are one.
    const float f = std::clamp(length(b.at - a.at) / (2.0f * std::max(d.radius, 1e-12f)), 0.5f, 1.0f);
    for (SculptDab* x : {&a, &b}) {
        if (x->tool == SculptDab::Tool::Grab) x->move *= f;
        else x->strength *= f;
    }
    return {a, b};
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
    return d.joined ? std::string("+") + text : std::string(text);
}

// --- the dabs on a geometry --------------------------------------------------------------

void sculpt(Geometry& geo, std::span<const SculptDab> dabs, Falloff shape, const Dyntopo& dyntopo) {
    if (geo.pointCount() == 0) return;
    if (dyntopo.on()) {
        SculptMesh mesh(geo);
        {
            DynamicSurface surface(mesh, dabs, dyntopo);
            apply(surface, dabs, shape);
        }
        geo = *mesh.geometry();
        renormal(geo);
        return;
    }
    if (dabs.empty()) return;
    // Who is smoothed towards whom, of the geometry as it came.
    std::unique_ptr<SculptSmoothing> smoothing;
    if (smooths(dabs)) smoothing = std::make_unique<SculptSmoothing>(geo);
    FixedSurface surface(geo, dabs, smoothing.get());
    if (apply(surface, dabs, shape)) renormal(geo);
}

// --- going on from where it got to ------------------------------------------------------

GeometryPtr Sculptor::cook(const GeometryPtr& source, std::span<const SculptDab> dabs, Falloff shape,
                           const Dyntopo& dyntopo) {
    reused_ = 0;
    if (!source || source->pointCount() == 0 || (dabs.empty() && !dyntopo.on())) return editableCopy(source);
    if (source != source_ || shape != shape_ || dyntopo != dyntopo_) {
        if (source != source_) smoothing_.reset();
        source_ = source;
        shape_ = shape;
        dyntopo_ = dyntopo;
        dabs_.clear();
        before_.reset();
        after_.reset();
        beforeMesh_.reset();
        afterMesh_.reset();
        movedBefore_ = movedAfter_ = false;
    }
    if (dyntopo.on()) return cookDynamic(dabs, shape);
    // How many of the dabs, from the first on, it had.
    size_t same = 0;
    while (same < dabs.size() && same < dabs_.size() && sameDab(dabs[same], dabs_[same])) ++same;
    std::shared_ptr<Geometry> geo;
    size_t from = 0;
    bool moved = false;
    const size_t oldLast = lastGroup(dabs_);
    if (after_ && same == dabs_.size() && (same == dabs.size() || !dabs[same].joined)) {
        if (same == dabs.size()) {
            reused_ = same;
            return after_;  // the same dabs: what it gave
        }
        geo = editableCopy(after_);  // more after them
        from = same;
        moved = movedAfter_;
    } else if (before_ && !dabs_.empty() && same >= oldLast && (oldLast == dabs.size() || !dabs[oldLast].joined)) {
        geo = editableCopy(before_);  // the last group other, more to it, or gone
        from = oldLast;
        moved = movedBefore_;
    } else {
        geo = editableCopy(source);
    }
    reused_ = from;
    if (!smoothing_ && smooths(dabs.subspan(from))) smoothing_ = std::make_shared<const SculptSmoothing>(*source);

    // All the new dabs but the last group; the geometry then kept, for the
    // next cook to go on from if only the last group changes. Where it went
    // on from past the start of the last group -- a group taken back --
    // there is none to keep.
    const size_t m = dabs.size();
    const size_t group = lastGroup(dabs);
    const size_t last = std::max(group, from);
    GeometryPtr before;
    bool movedBefore = false;
    if (from < m) {
        {
            FixedSurface surface(*geo, dabs.subspan(from, last - from), smoothing_.get());
            moved = apply(surface, dabs.subspan(from, last - from), shape) || moved;
        }
        if (group >= from) {
            before = std::make_shared<const Geometry>(*geo);
            movedBefore = moved;
        }
        FixedSurface surface(*geo, dabs.subspan(last), smoothing_.get());
        moved = apply(surface, dabs.subspan(last), shape) || moved;
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

GeometryPtr Sculptor::cookDynamic(std::span<const SculptDab> dabs, Falloff shape) {
    size_t same = 0;
    while (same < dabs.size() && same < dabs_.size() && sameDab(dabs[same], dabs_[same])) ++same;
    std::shared_ptr<SculptMesh> mesh;
    size_t from = 0;
    bool fromBefore = false;
    const size_t oldLast = lastGroup(dabs_);
    if (after_ && afterMesh_ && same == dabs_.size() && (same == dabs.size() || !dabs[same].joined)) {
        if (same == dabs.size()) {
            reused_ = same;
            return after_;
        }
        mesh = std::move(afterMesh_);  // more after them: goes on in place
        from = same;
    } else if (beforeMesh_ && !dabs_.empty() && same >= oldLast && (oldLast == dabs.size() || !dabs[oldLast].joined)) {
        mesh = std::make_shared<SculptMesh>(*beforeMesh_);  // the last group other, more to it, or gone
        from = oldLast;
        fromBefore = true;
    } else {
        mesh = std::make_shared<SculptMesh>(*source_);
    }
    reused_ = from;

    // All the new dabs but the last group; the mesh then kept, where the
    // last group is a Grab -- as it was, where that is the one that changed
    // -- then the last group. A mesh is no geometry sharing its buffers:
    // copied for every dab of a stroke, it would cost more than the dab.
    const size_t group = lastGroup(dabs);
    const size_t last = std::max(group, from);
    std::shared_ptr<const SculptMesh> before;
    {
        DynamicSurface surface(*mesh, dabs.subspan(from), dyntopo_);
        apply(surface, dabs.subspan(from, last - from), shape);
        if (from < dabs.size() && group >= from) {
            if (fromBefore && last == from) before = beforeMesh_;
            else if (dabs[last].tool == SculptDab::Tool::Grab) before = std::make_shared<const SculptMesh>(*mesh);
        }
        apply(surface, dabs.subspan(last), shape);
    }
    std::shared_ptr<Geometry> geo = mesh->geometry();
    renormal(*geo);
    beforeMesh_ = std::move(before);
    afterMesh_ = std::move(mesh);
    after_ = geo;
    dabs_.assign(dabs.begin(), dabs.end());
    return after_;
}

}  // namespace pg
