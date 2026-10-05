#include "pg/core/Pick.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pg {

namespace {

constexpr float kFar = std::numeric_limits<float>::infinity();

/// How far in front of a point a face may be and still not hide it: a
/// hair, the further the point is the more.
float slack(float distance) { return 1e-3f * distance + 1e-5f; }

/// Where the ray enters the box, if it does before `limit`. `inv` is 1 over
/// the ray's direction.
bool slab(const Vec3& lo, const Vec3& hi, const Vec3& origin, const Vec3& inv, float limit, float& enter) {
    float t0 = 0.0f, t1 = limit;
    for (int k = 0; k < 3; ++k) {
        float a = (lo[k] - origin[k]) * inv[k], b = (hi[k] - origin[k]) * inv[k];
        if (a > b) std::swap(a, b);
        // A NaN -- the ray along a face of the box -- limits nothing.
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    enter = t0;
    return true;
}

/// Pixels from (px, py) to the segment a-b on the screen, and where along
/// it (0 at a, 1 at b) the nearest point is.
float toSegment(float px, float py, float ax, float ay, float bx, float by, float& along) {
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    along = len2 > 1e-9f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    const float x = ax + dx * along - px, y = ay + dy * along - py;
    return std::sqrt(x * x + y * y);
}

/// The point of the segment a-b at `along` of the way on the screen: nearer
/// the end nearer the eye than halfway, as perspective has it.
Vec3 onSegment(const PickView& view, const Vec3& a, const Vec3& b, float along) {
    const float za = dot(a - view.eye, view.forward), zb = dot(b - view.eye, view.forward);
    const float den = (1.0f - along) * zb + along * za;
    const float t = std::fabs(den) > 1e-12f ? along * za / den : along;
    return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}

/// z of the cross product of two vectors on the screen.
float cross2(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

}  // namespace

// --- parts of the screen -------------------------------------------------------------------

ScreenRegion ScreenRegion::box(float x0, float y0, float x1, float y1) {
    ScreenRegion r;
    r.kind_ = Kind::Box;
    r.x0_ = std::min(x0, x1);
    r.y0_ = std::min(y0, y1);
    r.x1_ = std::max(x0, x1);
    r.y1_ = std::max(y0, y1);
    return r;
}

ScreenRegion ScreenRegion::lasso(std::vector<Vec2> points) {
    ScreenRegion r;
    r.kind_ = Kind::Lasso;
    r.points_ = std::move(points);
    const size_t n = r.points_.size();
    if (n == 0) return r;
    r.x0_ = r.x1_ = r.points_[0].x;
    r.y0_ = r.y1_ = r.points_[0].y;
    for (const Vec2& p : r.points_) {
        r.x0_ = std::min(r.x0_, p.x);
        r.x1_ = std::max(r.x1_, p.x);
        r.y0_ = std::min(r.y0_, p.y);
        r.y1_ = std::max(r.y1_, p.y);
    }
    if (n < 3) return r;
    // Each side into every band its height reaches.
    const size_t bands = std::clamp<size_t>(n / 4, 1, 256);
    r.bandTop_ = r.y0_;
    r.bandHeight_ = std::max((r.y1_ - r.y0_) / static_cast<float>(bands), 1e-6f);
    auto band = [&r, bands](float y) {
        const float b = (y - r.bandTop_) / r.bandHeight_;
        return b <= 0.0f ? size_t(0) : std::min(static_cast<size_t>(b), bands - 1);
    };
    std::vector<uint32_t> count(bands + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        const float ya = r.points_[i].y, yb = r.points_[(i + 1) % n].y;
        for (size_t b = band(std::min(ya, yb)); b <= band(std::max(ya, yb)); ++b) ++count[b + 1];
    }
    r.bandStart_.assign(bands + 1, 0);
    for (size_t b = 0; b < bands; ++b) r.bandStart_[b + 1] = r.bandStart_[b] + count[b + 1];
    r.bandSides_.assign(r.bandStart_[bands], 0);
    std::vector<uint32_t> at(r.bandStart_.begin(), r.bandStart_.end() - 1);
    for (size_t i = 0; i < n; ++i) {
        const float ya = r.points_[i].y, yb = r.points_[(i + 1) % n].y;
        for (size_t b = band(std::min(ya, yb)); b <= band(std::max(ya, yb)); ++b) {
            r.bandSides_[at[b]++] = static_cast<uint32_t>(i);
        }
    }
    return r;
}

ScreenRegion ScreenRegion::brush(float x0, float y0, float x1, float y1, float radius) {
    ScreenRegion r;
    r.kind_ = Kind::Brush;
    r.x0_ = x0;
    r.y0_ = y0;
    r.x1_ = x1;
    r.y1_ = y1;
    r.radius_ = std::max(radius, 0.0f);
    return r;
}

bool ScreenRegion::contains(float x, float y) const {
    switch (kind_) {
        case Kind::Box: return x >= x0_ && x <= x1_ && y >= y0_ && y <= y1_;
        case Kind::Brush: {
            float along = 0.0f;
            return toSegment(x, y, x0_, y0_, x1_, y1_, along) <= radius_;
        }
        case Kind::Lasso: {
            if (bandStart_.size() < 2 || x < x0_ || x > x1_ || y < y0_ || y > y1_) return false;
            const size_t bands = bandStart_.size() - 1;
            const float fb = (y - bandTop_) / bandHeight_;
            const size_t b = fb <= 0.0f ? 0 : std::min(static_cast<size_t>(fb), bands - 1);
            const size_t n = points_.size();
            bool in = false;
            for (uint32_t k = bandStart_[b]; k < bandStart_[b + 1]; ++k) {
                const size_t i = bandSides_[k];
                const Vec2& a = points_[i];
                const Vec2& c = points_[(i + 1) % n];
                if ((a.y > y) != (c.y > y) && x < (c.x - a.x) * (y - a.y) / (c.y - a.y) + a.x) in = !in;
            }
            return in;
        }
    }
    return false;
}

bool ScreenRegion::touches(float ax, float ay, float bx, float by, float& along) const {
    along = 0.5f;
    if (kind_ != Kind::Brush) return contains(ax, ay) && contains(bx, by);
    // The segments cross: there.
    const float ux = bx - ax, uy = by - ay, vx = x1_ - x0_, vy = y1_ - y0_;
    const float den = cross2(ux, uy, vx, vy);
    if (std::fabs(den) > 1e-12f) {
        const float t = cross2(x0_ - ax, y0_ - ay, vx, vy) / den;
        const float s = cross2(x0_ - ax, y0_ - ay, ux, uy) / den;
        if (t >= 0.0f && t <= 1.0f && s >= 0.0f && s <= 1.0f) {
            along = t;
            return true;
        }
    }
    // Else nearest where one of the four ends is.
    float best = 0.0f, t = 0.0f;
    best = toSegment(ax, ay, x0_, y0_, x1_, y1_, t);
    along = 0.0f;
    float d = toSegment(bx, by, x0_, y0_, x1_, y1_, t);
    if (d < best) {
        best = d;
        along = 1.0f;
    }
    d = toSegment(x0_, y0_, ax, ay, bx, by, t);
    if (d < best) {
        best = d;
        along = t;
    }
    d = toSegment(x1_, y1_, ax, ay, bx, by, t);
    if (d < best) {
        best = d;
        along = t;
    }
    return best <= radius_;
}

void ScreenRegion::bounds(float& x0, float& y0, float& x1, float& y1) const {
    if (kind_ == Kind::Brush) {
        x0 = std::min(x0_, x1_) - radius_;
        y0 = std::min(y0_, y1_) - radius_;
        x1 = std::max(x0_, x1_) + radius_;
        y1 = std::max(y0_, y1_) + radius_;
        return;
    }
    x0 = x0_;
    y0 = y0_;
    x1 = x1_;
    y1 = y1_;
}

// --- the view ----------------------------------------------------------------------------

bool PickView::project(const Vec3& p, float& sx, float& sy) const {
    const Vec3 v = p - eye;
    const float z = dot(v, forward);
    if (z <= 1e-4f) return false;
    const float aspect = width / std::max(height, 1.0f);
    const float px = dot(v, right) / (z * tanHalfFov * aspect);
    const float py = dot(v, up) / (z * tanHalfFov);
    sx = x + (px + 1.0f) * 0.5f * width;
    sy = y + (1.0f - py) * 0.5f * height;
    return true;
}

void PickView::ray(float sx, float sy, Vec3& origin, Vec3& dir) const {
    const float aspect = width / std::max(height, 1.0f);
    const float px = (sx - x) / std::max(width, 1.0f) * 2.0f - 1.0f;
    const float py = 1.0f - (sy - y) / std::max(height, 1.0f) * 2.0f;
    origin = eye;
    dir = normalize(forward + right * (px * tanHalfFov * aspect) + up * (py * tanHalfFov));
}

// --- the tree ----------------------------------------------------------------------------

void ElementPicker::build(GeometryPtr geo) {
    geo_ = std::move(geo);
    positions_ = corners_ = nullptr;
    pointCount_ = primitiveCount_ = 0;
    tris_.clear();
    nodes_.clear();
    edges_.clear();
    if (!geo_) return;
    const Geometry& g = *geo_;
    const auto P = g.positions();
    positions_ = P.data();
    corners_ = g.vertexPoints().data();
    pointCount_ = g.pointCount();
    primitiveCount_ = g.primitiveCount();
    // The faces: a fan across each closed polygon.
    for (size_t p = 0; p < primitiveCount_; ++p) {
        const auto pts = g.primitivePoints(p);
        if (pts.size() < 3 || !g.primitiveClosed(p)) continue;
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            Triangle t;
            t.a = pts[0];
            t.b = pts[k];
            t.c = pts[k + 1];
            if (t.a >= pointCount_ || t.b >= pointCount_ || t.c >= pointCount_) continue;
            t.prim = static_cast<uint32_t>(p);
            t.order = static_cast<uint32_t>(tris_.size());
            t.center = (P[t.a] + P[t.b] + P[t.c]) * (1.0f / 3.0f);
            tris_.push_back(t);
        }
    }
    if (!tris_.empty()) {
        nodes_.reserve(tris_.size() / 2 + 1);
        make(0, static_cast<uint32_t>(tris_.size()));
    }
    edges_ = edgesOf(g);
    vertexPrim_.assign(g.vertexCount(), 0);
    for (size_t p = 0; p < primitiveCount_; ++p) {
        const size_t first = g.primitiveVertexStart(p);
        for (size_t k = 0; k < g.primitiveVertexCount(p); ++k) vertexPrim_[first + k] = static_cast<uint32_t>(p);
    }
    builtArea_ = area_ = area();
}

bool ElementPicker::fits(const Geometry& geo) const {
    return geo_ && geo.pointCount() == pointCount_ && geo.primitiveCount() == primitiveCount_ &&
           geo.positions().data() == positions_ && geo.vertexPoints().data() == corners_;
}

bool ElementPicker::refit(GeometryPtr geo) {
    if (!geo_ || !geo || geo->pointCount() != pointCount_ || geo->primitiveCount() != primitiveCount_ ||
        geo->vertexPoints().data() != corners_) {
        return false;
    }
    geo_ = std::move(geo);
    const auto P = geo_->positions();
    positions_ = P.data();
    // A node's children come after it: from the last node back, each box
    // is made of boxes made already.
    for (size_t i = nodes_.size(); i-- > 0;) {
        Node& node = nodes_[i];
        if (node.count > 0) {
            node.lo = Vec3(kFar, kFar, kFar);
            node.hi = Vec3(-kFar, -kFar, -kFar);
            for (uint32_t k = node.first; k < node.first + node.count; ++k) {
                const Triangle& t = tris_[k];
                for (const uint32_t q : {t.a, t.b, t.c}) {
                    for (int a = 0; a < 3; ++a) {
                        node.lo[a] = std::min(node.lo[a], P[q][a]);
                        node.hi[a] = std::max(node.hi[a], P[q][a]);
                    }
                }
            }
        } else {
            const Node& l = nodes_[node.left];
            const Node& r = nodes_[node.right];
            for (int a = 0; a < 3; ++a) {
                node.lo[a] = std::min(l.lo[a], r.lo[a]);
                node.hi[a] = std::max(l.hi[a], r.hi[a]);
            }
        }
    }
    area_ = area();
    return true;
}

float ElementPicker::swell() const {
    if (builtArea_ > 0.0) return static_cast<float>(area_ / builtArea_);
    return area_ > 0.0 ? std::numeric_limits<float>::infinity() : 1.0f;
}

double ElementPicker::area() const {
    double sum = 0.0;
    for (const Node& node : nodes_) {
        const Vec3 d = node.hi - node.lo;
        sum += 2.0 * (static_cast<double>(d.x) * d.y + static_cast<double>(d.y) * d.z + static_cast<double>(d.z) * d.x);
    }
    return sum;
}

uint32_t ElementPicker::make(uint32_t first, uint32_t count) {
    const uint32_t id = static_cast<uint32_t>(nodes_.size());
    nodes_.emplace_back();
    const auto P = geo_->positions();
    Node node;
    node.lo = Vec3(kFar, kFar, kFar);
    node.hi = Vec3(-kFar, -kFar, -kFar);
    Vec3 clo(kFar, kFar, kFar), chi(-kFar, -kFar, -kFar);
    for (uint32_t i = first; i < first + count; ++i) {
        const Triangle& t = tris_[i];
        for (const uint32_t q : {t.a, t.b, t.c}) {
            for (int k = 0; k < 3; ++k) {
                node.lo[k] = std::min(node.lo[k], P[q][k]);
                node.hi[k] = std::max(node.hi[k], P[q][k]);
            }
        }
        for (int k = 0; k < 3; ++k) {
            clo[k] = std::min(clo[k], t.center[k]);
            chi[k] = std::max(chi[k], t.center[k]);
        }
    }
    if (count <= 4) {
        node.first = first;
        node.count = count;
        nodes_[id] = node;
        return id;
    }
    // Halved across the longest side of the middles' box.
    const Vec3 extent = chi - clo;
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
    const uint32_t half = count / 2;
    std::nth_element(tris_.begin() + first, tris_.begin() + first + half, tris_.begin() + first + count,
                     [axis](const Triangle& a, const Triangle& b) {
                         return a.center[axis] < b.center[axis] || (a.center[axis] == b.center[axis] && a.order < b.order);
                     });
    node.left = make(first, half);
    node.right = make(first + half, count - half);
    nodes_[id] = node;
    return id;
}

bool ElementPicker::hit(const Triangle& tri, const Vec3& origin, const Vec3& dir, float& best) const {
    const auto P = geo_->positions();
    const Vec3& a = P[tri.a];
    const Vec3 e1 = P[tri.b] - a, e2 = P[tri.c] - a;
    const Vec3 h = cross(dir, e2);
    const float det = dot(e1, h);
    if (std::fabs(det) < 1e-14f) return false;
    const float inv = 1.0f / det;
    const Vec3 s = origin - a;
    const float u = dot(s, h) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = cross(s, e1);
    const float v = dot(dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = dot(e2, q) * inv;
    if (!(t > 1e-7f) || t > best) return false;
    best = t;
    return true;
}

int32_t ElementPicker::raycast(const Vec3& origin, const Vec3& dir, float& t, Vec3* normal) const {
    t = kFar;
    if (nodes_.empty()) return -1;
    const Vec3 inv(1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z);
    uint32_t stack[96];
    int top = 0;
    stack[top++] = 0;
    const Triangle* best = nullptr;
    float bestT = kFar;
    while (top > 0) {
        const Node& n = nodes_[stack[--top]];
        float enter = 0.0f;
        if (!slab(n.lo, n.hi, origin, inv, bestT, enter)) continue;
        if (n.count > 0) {
            for (uint32_t i = n.first; i < n.first + n.count; ++i) {
                float at = bestT;
                if (!hit(tris_[i], origin, dir, at)) continue;
                // As far as the best so far: the face made first.
                if (at < bestT || !best || tris_[i].order < best->order) {
                    bestT = at;
                    best = &tris_[i];
                }
            }
            continue;
        }
        // The nearer child asked first: last on the stack.
        float enterLeft = 0.0f, enterRight = 0.0f;
        const Node& l = nodes_[n.left];
        const Node& r = nodes_[n.right];
        const bool left = slab(l.lo, l.hi, origin, inv, bestT, enterLeft);
        const bool right = slab(r.lo, r.hi, origin, inv, bestT, enterRight);
        if (left && right) {
            stack[top++] = enterLeft <= enterRight ? n.right : n.left;
            stack[top++] = enterLeft <= enterRight ? n.left : n.right;
        } else if (left) {
            stack[top++] = n.left;
        } else if (right) {
            stack[top++] = n.right;
        }
    }
    if (!best) return -1;
    t = bestT;
    if (normal) {
        const auto P = geo_->positions();
        Vec3 nn = normalize(cross(P[best->b] - P[best->a], P[best->c] - P[best->a]));
        if (dot(nn, dir) > 0.0f) nn = nn * -1.0f;
        *normal = nn;
    }
    return static_cast<int32_t>(best->prim);
}

bool ElementPicker::visible(const Vec3& eye, const Vec3& p) const {
    const Vec3 d = p - eye;
    const float dist = length(d);
    if (dist < 1e-9f) return true;
    float t = 0.0f;
    return raycast(eye, d * (1.0f / dist), t) < 0 || t >= dist - slack(dist);
}

bool ElementPicker::faceSeen(size_t prim, const Vec3& eye, const Vec3& p) const {
    // A polygon's middle is on it -- or, bent, near it: seen when the ray
    // to it meets the polygon itself first, or nothing in front of it.
    const Vec3 d = p - eye;
    const float dist = length(d);
    if (dist < 1e-9f) return true;
    float t = 0.0f;
    const int32_t first = raycast(eye, d * (1.0f / dist), t);
    return first < 0 || first == static_cast<int32_t>(prim) || t >= dist - slack(dist);
}

Vec3 ElementPicker::middle(size_t prim) const {
    if (!geo_ || prim >= geo_->primitiveCount()) return Vec3();
    const auto pts = geo_->primitivePoints(prim);
    const auto P = geo_->positions();
    Vec3 sum;
    size_t n = 0;
    for (const uint32_t q : pts) {
        if (q >= P.size()) continue;
        sum += P[q];
        ++n;
    }
    return n ? sum * (1.0f / static_cast<float>(n)) : Vec3();
}

Vec3 ElementPicker::vertexMark(size_t v) const {
    if (!geo_ || v >= vertexPrim_.size()) return Vec3();
    const Geometry& g = *geo_;
    const auto P = g.positions();
    const uint32_t q = g.vertexPoints()[v];
    if (q >= P.size()) return Vec3();
    const size_t prim = vertexPrim_[v];
    if (!g.primitiveClosed(prim) || g.primitiveVertexCount(prim) < 3) return P[q];
    return P[q] + (middle(prim) - P[q]) * kVertexInset;
}

bool ElementPicker::vertexSeen(size_t v, const Vec3& eye, const Vec3& mark) const {
    const size_t prim = vertexPrim_[v];
    const Geometry& g = *geo_;
    if (g.primitiveClosed(prim) && g.primitiveVertexCount(prim) >= 3) return faceSeen(prim, eye, mark);
    return visible(eye, mark);
}

// --- under the mouse ----------------------------------------------------------------------

int32_t ElementPicker::point(const PickView& view, float sx, float sy, float reach, bool hidden) const {
    if (!geo_) return -1;
    const auto P = geo_->positions();
    std::vector<std::pair<float, uint32_t>> near;
    for (size_t i = 0; i < P.size(); ++i) {
        float x = 0.0f, y = 0.0f;
        if (!view.project(P[i], x, y)) continue;
        const float d2 = (x - sx) * (x - sx) + (y - sy) * (y - sy);
        if (d2 <= reach * reach) near.emplace_back(d2, static_cast<uint32_t>(i));
    }
    // The nearest on the screen that is not hidden.
    std::sort(near.begin(), near.end());
    for (const auto& [d2, i] : near) {
        if (hidden || visible(view.eye, P[i])) return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t ElementPicker::edge(const PickView& view, float sx, float sy, float reach, bool hidden) const {
    if (!geo_) return -1;
    const auto P = geo_->positions();
    struct Near {
        float pixels, along;
        uint32_t edge;
    };
    std::vector<Near> near;
    for (size_t i = 0; i < edges_.size(); ++i) {
        const Edge& e = edges_[i];
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        if (!view.project(P[e.first], ax, ay) || !view.project(P[e.second], bx, by)) continue;
        // Not near the box round it: not near it.
        if (sx < std::min(ax, bx) - reach || sx > std::max(ax, bx) + reach || sy < std::min(ay, by) - reach ||
            sy > std::max(ay, by) + reach) {
            continue;
        }
        float along = 0.0f;
        const float pixels = toSegment(sx, sy, ax, ay, bx, by, along);
        if (pixels <= reach) near.push_back({pixels, along, static_cast<uint32_t>(i)});
    }
    std::sort(near.begin(), near.end(),
              [](const Near& a, const Near& b) { return a.pixels < b.pixels || (a.pixels == b.pixels && a.edge < b.edge); });
    for (const Near& n : near) {
        const Edge& e = edges_[n.edge];
        if (hidden || visible(view.eye, onSegment(view, P[e.first], P[e.second], n.along))) {
            return static_cast<int32_t>(n.edge);
        }
    }
    return -1;
}

int32_t ElementPicker::primitive(const PickView& view, float sx, float sy, float reach, bool hidden) const {
    if (!geo_) return -1;
    Vec3 origin, dir;
    view.ray(sx, sy, origin, dir);
    float t = kFar;
    const int32_t face = raycast(origin, dir, t);
    // A polyline is thin: one within reach in front of the face wins.
    const Geometry& g = *geo_;
    const auto P = g.positions();
    int32_t curve = -1;
    float nearest = reach;
    for (size_t p = 0; p < g.primitiveCount(); ++p) {
        const auto pts = g.primitivePoints(p);
        if (pts.size() < 2 || (g.primitiveClosed(p) && pts.size() >= 3)) continue;
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const Vec3& a = P[pts[k]];
            const Vec3& b = P[pts[k + 1]];
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            if (!view.project(a, ax, ay) || !view.project(b, bx, by)) continue;
            float along = 0.0f;
            const float pixels = toSegment(sx, sy, ax, ay, bx, by, along);
            if (pixels > nearest) continue;
            const Vec3 q = onSegment(view, a, b, along);
            if (!hidden && (length(q - view.eye) > t + slack(t) || !visible(view.eye, q))) continue;
            nearest = pixels;
            curve = static_cast<int32_t>(p);
        }
    }
    return curve >= 0 ? curve : face;
}

int32_t ElementPicker::vertex(const PickView& view, float sx, float sy, float reach, bool hidden) const {
    if (!geo_) return -1;
    std::vector<std::pair<float, uint32_t>> near;
    for (size_t v = 0; v < vertexPrim_.size(); ++v) {
        float x = 0.0f, y = 0.0f;
        if (!view.project(vertexMark(v), x, y)) continue;
        const float d2 = (x - sx) * (x - sx) + (y - sy) * (y - sy);
        if (d2 <= reach * reach) near.emplace_back(d2, static_cast<uint32_t>(v));
    }
    // The nearest on the screen that is not hidden.
    std::sort(near.begin(), near.end());
    for (const auto& [d2, v] : near) {
        if (hidden || vertexSeen(v, view.eye, vertexMark(v))) return static_cast<int32_t>(v);
    }
    return -1;
}

// --- in a part of the screen ------------------------------------------------------------

std::vector<uint8_t> ElementPicker::pointsIn(const PickView& view, const ScreenRegion& region, bool hidden) const {
    if (!geo_) return {};
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    region.bounds(x0, y0, x1, y1);
    const auto P = geo_->positions();
    std::vector<uint8_t> out(P.size(), 0);
    parallelFor(P.size(), 512, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            float x = 0.0f, y = 0.0f;
            if (!view.project(P[i], x, y) || x < x0 || x > x1 || y < y0 || y > y1 || !region.contains(x, y)) continue;
            out[i] = hidden || visible(view.eye, P[i]) ? 1 : 0;
        }
    });
    return out;
}

std::vector<uint8_t> ElementPicker::verticesIn(const PickView& view, const ScreenRegion& region, bool hidden) const {
    if (!geo_) return {};
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    region.bounds(x0, y0, x1, y1);
    std::vector<uint8_t> out(vertexPrim_.size(), 0);
    parallelFor(out.size(), 512, [&](size_t begin, size_t end) {
        for (size_t v = begin; v < end; ++v) {
            const Vec3 mark = vertexMark(v);
            float x = 0.0f, y = 0.0f;
            if (!view.project(mark, x, y) || x < x0 || x > x1 || y < y0 || y > y1 || !region.contains(x, y)) continue;
            out[v] = hidden || vertexSeen(v, view.eye, mark) ? 1 : 0;
        }
    });
    return out;
}

std::vector<uint8_t> ElementPicker::edgesIn(const PickView& view, const ScreenRegion& region, bool hidden) const {
    if (!geo_) return {};
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    region.bounds(x0, y0, x1, y1);
    const bool brush = region.kind() == ScreenRegion::Kind::Brush;
    const auto P = geo_->positions();
    std::vector<uint8_t> out(edges_.size(), 0);
    parallelFor(edges_.size(), 512, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const Edge& e = edges_[i];
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            if (!view.project(P[e.first], ax, ay) || !view.project(P[e.second], bx, by)) continue;
            if (std::max(ax, bx) < x0 || std::min(ax, bx) > x1 || std::max(ay, by) < y0 || std::min(ay, by) > y1) continue;
            float along = 0.5f;
            if (!region.touches(ax, ay, bx, by, along)) continue;
            // Seen where the brush touches it; taken in whole, at its middle.
            const Vec3 at = brush ? onSegment(view, P[e.first], P[e.second], along) : (P[e.first] + P[e.second]) * 0.5f;
            out[i] = hidden || visible(view.eye, at) ? 1 : 0;
        }
    });
    return out;
}

std::vector<uint8_t> ElementPicker::primitivesIn(const PickView& view, const ScreenRegion& region, bool hidden) const {
    if (!geo_) return {};
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    region.bounds(x0, y0, x1, y1);
    const bool brush = region.kind() == ScreenRegion::Kind::Brush;
    const Geometry& g = *geo_;
    const auto P = g.positions();
    std::vector<uint8_t> out(g.primitiveCount(), 0);
    parallelFor(out.size(), 256, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            const bool face = g.primitiveClosed(p) && g.primitiveVertexCount(p) >= 3;
            if (brush && !face) {
                // A polyline: where the brush touches it, if it is seen there.
                const auto pts = g.primitivePoints(p);
                for (size_t k = 0; k + 1 < pts.size() && !out[p]; ++k) {
                    if (pts[k] >= P.size() || pts[k + 1] >= P.size()) continue;
                    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f, along = 0.0f;
                    if (!view.project(P[pts[k]], ax, ay) || !view.project(P[pts[k + 1]], bx, by)) continue;
                    if (!region.touches(ax, ay, bx, by, along)) continue;
                    out[p] = hidden || visible(view.eye, onSegment(view, P[pts[k]], P[pts[k + 1]], along)) ? 1 : 0;
                }
                continue;
            }
            const Vec3 m = middle(p);
            float x = 0.0f, y = 0.0f;
            if (!view.project(m, x, y) || x < x0 || x > x1 || y < y0 || y > y1 || !region.contains(x, y)) continue;
            out[p] = hidden || (face ? faceSeen(p, view.eye, m) : visible(view.eye, m)) ? 1 : 0;
        }
    });
    if (brush) {
        // The faces under the brush's middle, all along its way: a face
        // larger than the brush may have no middle in it.
        const Vec2 a = region.from(), b = region.to();
        const float step = std::max(0.5f * region.radius(), 1.0f);
        const int n = std::min(static_cast<int>(std::ceil(std::hypot(b.x - a.x, b.y - a.y) / step)), 1024);
        for (int k = 0; k <= n; ++k) {
            const float f = n ? static_cast<float>(k) / static_cast<float>(n) : 0.0f;
            Vec3 origin, dir;
            view.ray(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, origin, dir);
            float t = 0.0f;
            const int32_t hit = raycast(origin, dir, t);
            if (hit >= 0) out[static_cast<size_t>(hit)] = 1;
        }
    }
    return out;
}

}  // namespace pg
