#include "pg/grammar/Shape.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <utility>

namespace pg::grammar {
namespace {

/// Shapes per chunk of a rewriting pass.
constexpr size_t kGrain = 256;

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

std::span<const Vec3> vec3Attribute(const AttributeSet& set, const char* name) {
    const AttributeArray* a = set.find(name);
    return a && a->type() == AttrType::Vec3 ? a->read<Vec3>() : std::span<const Vec3>();
}

const std::string& emptyString() {
    static const std::string s;
    return s;
}

}  // namespace

const char* assetName(Asset a) {
    switch (a) {
        case Asset::Box:    return "";
        case Asset::Recess: return "recess";
        case Asset::Hip:    return "hip";
        case Asset::Gable:  return "gable";
    }
    return "";
}

Asset assetFromName(const std::string& name) {
    if (name == "recess") return Asset::Recess;
    if (name == "hip") return Asset::Hip;
    if (name == "gable") return Asset::Gable;
    return Asset::Box;
}

int flatAxis(const Scope& s) {
    if (s.size.y <= kEpsilon) return 1;
    if (s.size.z <= kEpsilon) return 2;
    if (s.size.x <= kEpsilon) return 0;
    return -1;
}

// --- operations ------------------------------------------------------------

const char* faceName(Face f) {
    switch (f) {
        case Face::Front:  return "front";
        case Face::Right:  return "right";
        case Face::Back:   return "back";
        case Face::Left:   return "left";
        case Face::Top:    return "top";
        case Face::Bottom: return "bottom";
    }
    return "?";
}

Scope faceScope(const Scope& s, Face f) {
    // Each face frame is right-handed, with x running left to right as seen by
    // someone standing outside the face and z pointing at them. The origins are
    // chosen so that the face covers [0, size.x] x [0, size.y] of its frame.
    const Vec3 &X = s.x, &Y = s.y, &Z = s.z;
    const float sx = s.size.x, sy = s.size.y, sz = s.size.z;
    Scope r;
    switch (f) {
        case Face::Front:
            r.origin = s.at(0, 0, sz); r.x = X;  r.y = Y;  r.z = Z;  r.size = {sx, sy, 0};
            break;
        case Face::Right:
            r.origin = s.at(sx, 0, sz); r.x = -Z; r.y = Y;  r.z = X;  r.size = {sz, sy, 0};
            break;
        case Face::Back:
            r.origin = s.at(sx, 0, 0); r.x = -X; r.y = Y;  r.z = -Z; r.size = {sx, sy, 0};
            break;
        case Face::Left:
            r.origin = s.at(0, 0, 0);  r.x = Z;  r.y = Y;  r.z = -X; r.size = {sz, sy, 0};
            break;
        case Face::Top:  // seen from above with the front at the bottom
            r.origin = s.at(0, sy, sz); r.x = X;  r.y = -Z; r.z = Y;  r.size = {sx, sz, 0};
            break;
        case Face::Bottom:
            r.origin = s.at(sx, 0, sz); r.x = -X; r.y = -Z; r.z = -Y; r.size = {sx, sz, 0};
            break;
    }
    return r;
}

Scope faceFrame(const Scope& s) {
    switch (flatAxis(s)) {
        case 1:  return faceScope(s, Face::Top);    // a footprint: its upper side
        case 0:  return faceScope(s, Face::Right);  // flat along x: its +x side
        default: return s;                          // already faces along z
    }
}

Shape extrude(const Shape& s, float distance) {
    Shape out = s;
    const float d = std::fabs(distance);
    const int axis = flatAxis(s.scope);

    if (axis < 0) {
        // Already a volume: extrusion sets its height, downwards if negative.
        out.scope.size.y = d;
        if (distance < 0.0f) out.scope.origin = s.scope.origin - s.scope.y * d;
        return out;
    }
    if (distance >= 0.0f) {
        out.scope.size[axis] = d;
        return out;
    }
    // A recess is the volume behind the shape. It is framed so that its opening
    // is always +z, whichever axis the shape was flat along -- the mesher then
    // needs to know only one convention.
    out.scope = faceFrame(s.scope);
    out.scope.origin = out.scope.origin - out.scope.z * d;
    out.scope.size.z = d;
    out.asset = Asset::Recess;
    return out;
}

Shape roof(const Shape& s, RoofType type, float angleDegrees) {
    Shape out;
    out.scope = flatAxis(s.scope) < 0 ? faceScope(s.scope, Face::Top) : faceFrame(s.scope);
    const float angle = std::clamp(angleDegrees, 0.0f, 89.0f);
    const float span = std::min(out.scope.size.x, out.scope.size.y);
    out.scope.size.z = std::tan(angle * kDegToRad) * span * 0.5f;
    out.asset = type == RoofType::Hip ? Asset::Hip : Asset::Gable;
    return out;
}

// --- split -----------------------------------------------------------------

std::vector<SplitPiece> layoutSplit(const SplitPattern& p, float length) {
    std::vector<SplitPiece> out;
    if (length <= kEpsilon) return out;

    auto fixedOf = [&](const SplitPart& s) {
        switch (s.mode) {
            case SizeMode::Absolute: return s.value;
            case SizeMode::Relative: return s.value * length;
            case SizeMode::Floating: return 0.0f;
        }
        return 0.0f;
    };
    auto floatingOf = [](const SplitPart& s) {
        return s.mode == SizeMode::Floating ? s.value : 0.0f;
    };
    auto sum = [](const std::vector<SplitPart>& parts, auto fn) {
        float total = 0.0f;
        for (const auto& s : parts) total += fn(s);
        return total;
    };

    const float outerFixed = sum(p.head, fixedOf) + sum(p.tail, fixedOf);
    const float outerFloating = sum(p.head, floatingOf) + sum(p.tail, floatingOf);
    const float unitFixed = sum(p.repeat, fixedOf);
    const float unitFloating = sum(p.repeat, floatingOf);

    // How many times the repeat group is tiled. With floating parts it is the
    // count that stretches them least; with only fixed parts, as many whole
    // tiles as fit.
    int repeats = 0;
    const float unit = unitFixed + unitFloating;
    const float room = length - outerFixed - outerFloating;
    if (!p.repeat.empty() && unit > kEpsilon && room > kEpsilon) {
        const float fit = room / unit;
        repeats = unitFloating > 0.0f ? std::max(1, static_cast<int>(std::floor(fit + 0.5f)))
                                      : static_cast<int>(std::floor(fit + 1e-4f));
        repeats = std::min(repeats, kMaxRepeats);
    }

    // Floating parts share whatever the fixed ones leave, in proportion.
    const float fixedTotal = outerFixed + unitFixed * static_cast<float>(repeats);
    const float floatingTotal = outerFloating + unitFloating * static_cast<float>(repeats);
    const float stretch =
        floatingTotal > 0.0f ? std::max(0.0f, (length - fixedTotal) / floatingTotal) : 0.0f;

    float cursor = 0.0f;
    auto place = [&](const SplitPart& part) {
        if (cursor >= length) return;
        float len = part.mode == SizeMode::Floating ? part.value * stretch : fixedOf(part);
        len = std::min(len, length - cursor);  // clip what does not fit
        if (len > kEpsilon) out.push_back(SplitPiece{cursor, len, &part});
        cursor += len;
    };
    for (const auto& part : p.head) place(part);
    for (int r = 0; r < repeats; ++r) {
        for (const auto& part : p.repeat) place(part);
    }
    for (const auto& part : p.tail) place(part);
    return out;
}

// --- ShapeView -------------------------------------------------------------

ShapeView::ShapeView(const Geometry& geo) : geo_(&geo), count_(geo.pointCount()) {
    const AttributeSet& pts = geo.points();
    origin_ = geo.positions();
    x_ = vec3Attribute(pts, kAttrX);
    y_ = vec3Attribute(pts, kAttrY);
    z_ = vec3Attribute(pts, kAttrZ);
    size_ = vec3Attribute(pts, kAttrSize);

    if (const AttributeArray* a = pts.find(kAttrShape); a && a->type() == AttrType::String) {
        symbols_ = a;
        symbolIds_ = a->read<int32_t>();
    }
    if (const AttributeArray* a = pts.find(kAttrAsset); a && a->type() == AttrType::String) {
        assetIds_ = a->read<int32_t>();
        assetById_.resize(a->stringTableSize());
        for (size_t t = 0; t < assetById_.size(); ++t) {
            assetById_[t] = assetFromName(a->stringValue(static_cast<int32_t>(t)));
        }
    }
}

Shape ShapeView::shape(size_t i) const {
    Shape s;
    s.scope.origin = origin_[i];
    if (!x_.empty()) s.scope.x = x_[i];
    if (!y_.empty()) s.scope.y = y_[i];
    if (!z_.empty()) s.scope.z = z_[i];
    if (!size_.empty()) s.scope.size = size_[i];
    if (!assetIds_.empty()) {
        const int32_t id = assetIds_[i];
        if (id >= 0 && static_cast<size_t>(id) < assetById_.size()) {
            s.asset = assetById_[static_cast<size_t>(id)];
        }
    }
    return s;
}

int32_t ShapeView::symbolId(size_t i) const {
    if (symbolIds_.empty()) return -1;
    const int32_t id = symbolIds_[i];
    return id >= 0 && static_cast<size_t>(id) < symbolCount() ? id : -1;
}

size_t ShapeView::symbolCount() const { return symbols_ ? symbols_->stringTableSize() : 0; }

const std::string& ShapeView::symbolName(int32_t id) const {
    return symbols_ && id >= 0 ? symbols_->stringValue(id) : emptyString();
}

std::vector<uint8_t> ShapeView::select(const std::string& name) const {
    std::vector<uint8_t> out(count_, name.empty() ? 1 : 0);
    if (name.empty()) return out;

    // Compare each distinct symbol once, not each shape.
    std::vector<uint8_t> matches(symbolCount(), 0);
    for (size_t t = 0; t < matches.size(); ++t) {
        matches[t] = symbolName(static_cast<int32_t>(t)) == name ? 1 : 0;
    }
    for (size_t i = 0; i < count_; ++i) {
        const int32_t id = symbolId(i);
        out[i] = id >= 0 ? matches[static_cast<size_t>(id)] : 0;
    }
    return out;
}

// --- rewriting -------------------------------------------------------------

std::vector<Successor> rewrite(const ShapeView& shapes, const RewriteFn& fn) {
    const auto chunks = chunkRanges(shapes.size(), kGrain);
    std::vector<std::vector<Successor>> parts(chunks.size());

    TaskPool::instance().run(chunks.size(), [&](size_t c) {
        std::vector<Successor>& out = parts[c];
        for (size_t i = chunks[c].first; i < chunks[c].second; ++i) {
            if (!fn(i, out)) {
                out.push_back(Successor{static_cast<uint32_t>(i), shapes.shape(i), nullptr, false});
            }
        }
    });

    // The one sequential step: joining in chunk order is what makes the result
    // independent of which thread finished first.
    size_t total = 0;
    for (const auto& p : parts) total += p.size();
    std::vector<Successor> all;
    all.reserve(total);
    for (auto& p : parts) all.insert(all.end(), p.begin(), p.end());
    return all;
}

GeometryPtr materialize(const Geometry& in, const std::vector<Successor>& successors) {
    const size_t n = successors.size();
    auto out = std::make_shared<Geometry>();
    out->detail() = in.detail();

    // Inheritance: each successor starts as a copy of its parent's point, which
    // brings along every attribute anyone ever put on it. One gather per
    // attribute, whatever the rule was.
    std::vector<uint32_t> parents(n);
    for (size_t i = 0; i < n; ++i) parents[i] = successors[i].parent;
    AttributeSet points = in.points();
    points.gather(parents);
    out->points() = std::move(points);

    // Create everything before taking spans: creating an attribute of the
    // wrong type replaces it, which would leave a span dangling.
    AttributeSet& pts = out->points();
    for (const char* name : {"P", kAttrX, kAttrY, kAttrZ, kAttrSize}) {
        pts.create(name, AttrType::Vec3);
    }
    AttributeArray& symbols = pts.create(kAttrShape, AttrType::String);
    AttributeArray& paths = pts.create(kAttrPath, AttrType::String);
    AttributeArray& assets = pts.create(kAttrAsset, AttrType::String);

    // Strings are interned once per distinct value, not once per shape: a
    // thousand windows share one "Lot/Mass/Facade/Floor/Window".
    std::vector<int32_t> newSymbol(n, -1), newPath(n, -1), newAsset(n, 0);
    {
        std::unordered_map<const std::string*, int32_t> symbolIds;
        std::map<std::pair<int32_t, std::string>, int32_t> pathIds;
        int32_t assetIds[4] = {-1, -1, -1, -1};
        const auto parentPaths = paths.read<int32_t>();

        for (size_t i = 0; i < n; ++i) {
            const Successor& s = successors[i];
            int32_t& assetId = assetIds[static_cast<int>(s.shape.asset)];
            if (assetId < 0) assetId = assets.internString(assetName(s.shape.asset));
            newAsset[i] = assetId;

            if (!s.symbol) continue;
            auto [it, fresh] = symbolIds.emplace(s.symbol, -1);
            if (fresh) it->second = symbols.internString(*s.symbol);
            newSymbol[i] = it->second;

            const int32_t parentPath = parentPaths[i];
            auto [pit, pfresh] = pathIds.emplace(std::make_pair(parentPath, *s.symbol), -1);
            if (pfresh) {
                const std::string& base = paths.stringValue(parentPath);
                pit->second = paths.internString(base.empty() ? *s.symbol : base + "/" + *s.symbol);
            }
            newPath[i] = pit->second;
        }
    }

    auto P = pts.find("P")->write<Vec3>();
    auto X = pts.find(kAttrX)->write<Vec3>();
    auto Y = pts.find(kAttrY)->write<Vec3>();
    auto Z = pts.find(kAttrZ)->write<Vec3>();
    auto S = pts.find(kAttrSize)->write<Vec3>();
    auto sym = symbols.write<int32_t>();
    auto path = paths.write<int32_t>();
    auto asset = assets.write<int32_t>();
    for (size_t i = 0; i < n; ++i) {
        const Scope& sc = successors[i].shape.scope;
        P[i] = sc.origin;
        X[i] = sc.x;
        Y[i] = sc.y;
        Z[i] = sc.z;
        S[i] = sc.size;
        asset[i] = newAsset[i];
        if (newSymbol[i] >= 0) {
            sym[i] = newSymbol[i];
            path[i] = newPath[i];
        }
    }
    return out;
}

void emitSplit(uint32_t parent, const Shape& s, int axis, const SplitPattern& pattern,
               std::vector<Successor>& out) {
    const float length = s.scope.size[axis];
    if (length <= kEpsilon) {
        out.push_back(Successor{parent, s, nullptr, true});
        return;
    }
    const Vec3& dir = s.scope.axis(axis);
    for (const SplitPiece& piece : layoutSplit(pattern, length)) {
        if (piece.part->symbol == kNil) continue;
        Shape child = s;
        child.scope.origin = s.scope.origin + dir * piece.start;
        child.scope.size[axis] = piece.length;
        out.push_back(Successor{parent, child, &piece.part->symbol, true});
    }
}

const std::string* CompTargets::target(Face f) const {
    const std::string* t = &face[static_cast<int>(f)];
    const bool vertical = f != Face::Top && f != Face::Bottom;
    if (t->empty() && vertical) t = &side;
    return t->empty() || *t == kNil ? nullptr : t;
}

void emitComp(uint32_t parent, const Shape& s, const CompTargets& targets,
              std::vector<Successor>& out) {
    for (int f = 0; f < kFaceCount; ++f) {
        const std::string* target = targets.target(static_cast<Face>(f));
        if (!target) continue;
        Shape face{faceScope(s.scope, static_cast<Face>(f)), Asset::Box};
        if (face.scope.size.x <= kEpsilon || face.scope.size.y <= kEpsilon) continue;
        out.push_back(Successor{parent, face, target, true});
    }
}

// --- entering and leaving the shape world -----------------------------------

bool fitPolygon(std::span<const Vec3> pts, Scope& out) {
    if (pts.size() < 3) return false;

    // Newell's method: a normal that is robust for any planar polygon.
    Vec3 n(0.0f);
    for (size_t i = 0; i < pts.size(); ++i) {
        const Vec3& a = pts[i];
        const Vec3& b = pts[(i + 1) % pts.size()];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    if (length(n) <= kEpsilon) return false;
    n = normalize(n);
    if (n.y < 0.0f) n = -n;  // lots face up, whatever their winding

    // x follows the first edge that has a length, made exactly perpendicular.
    Vec3 x(0.0f);
    for (size_t i = 0; i + 1 < pts.size() && length(x) <= kEpsilon; ++i) {
        const Vec3 e = pts[i + 1] - pts[i];
        x = e - n * dot(e, n);
    }
    if (length(x) <= kEpsilon) return false;
    x = normalize(x);
    const Vec3 z = cross(x, n);

    float u0 = 0, u1 = 0, w0 = 0, w1 = 0;
    for (const Vec3& p : pts) {
        const Vec3 d = p - pts[0];
        u0 = std::min(u0, dot(d, x));
        u1 = std::max(u1, dot(d, x));
        w0 = std::min(w0, dot(d, z));
        w1 = std::max(w1, dot(d, z));
    }
    out.origin = pts[0] + x * u0 + z * w0;
    out.x = x;
    out.y = n;
    out.z = z;
    out.size = Vec3(u1 - u0, 0.0f, w1 - w0);
    return true;
}

namespace {

/// Polygons accumulated by mesh(), with the shape each one came from.
struct MeshBuilder {
    std::vector<Vec3> points;
    std::vector<uint32_t> corners;  ///< polygon corners, flattened
    std::vector<uint32_t> counts;   ///< corners per polygon
    std::vector<uint32_t> owners;   ///< shape index per polygon
    uint32_t owner = 0;

    uint32_t point(const Vec3& p) {
        points.push_back(p);
        return static_cast<uint32_t>(points.size() - 1);
    }

    void polygon(std::initializer_list<uint32_t> idx) {
        corners.insert(corners.end(), idx.begin(), idx.end());
        counts.push_back(static_cast<uint32_t>(idx.size()));
        owners.push_back(owner);
    }

    /// A face of a solid, wound so that its normal points away from `inside`
    /// -- or towards it, for the faces of a hole.
    void face(std::initializer_list<uint32_t> idx, const Vec3& inside, bool inwards) {
        std::vector<uint32_t> ring(idx);
        Vec3 n(0.0f), centre(0.0f);
        for (size_t i = 0; i < ring.size(); ++i) {
            const Vec3& a = points[ring[i]];
            const Vec3& b = points[ring[(i + 1) % ring.size()]];
            n.x += (a.y - b.y) * (a.z + b.z);
            n.y += (a.z - b.z) * (a.x + b.x);
            n.z += (a.x - b.x) * (a.y + b.y);
            centre += a;
        }
        centre = centre * (1.0f / static_cast<float>(ring.size()));
        const float facing = dot(n, centre - inside);
        if (inwards ? facing > 0.0f : facing < 0.0f) std::reverse(ring.begin(), ring.end());
        corners.insert(corners.end(), ring.begin(), ring.end());
        counts.push_back(static_cast<uint32_t>(ring.size()));
        owners.push_back(owner);
    }
};

void meshBox(const Scope& s, bool recess, MeshBuilder& mb) {
    const float sx = s.size.x, sy = s.size.y, sz = s.size.z;
    uint32_t c[8];
    for (int i = 0; i < 8; ++i) {
        c[i] = mb.point(s.at((i & 1) ? sx : 0.0f, (i & 2) ? sy : 0.0f, (i & 4) ? sz : 0.0f));
    }
    const Vec3 inside = s.at(sx * 0.5f, sy * 0.5f, sz * 0.5f);
    mb.face({c[0], c[2], c[6], c[4]}, inside, recess);  // x = 0
    mb.face({c[1], c[5], c[7], c[3]}, inside, recess);  // x = sx
    mb.face({c[0], c[4], c[5], c[1]}, inside, recess);  // y = 0
    mb.face({c[2], c[3], c[7], c[6]}, inside, recess);  // y = sy
    mb.face({c[0], c[1], c[3], c[2]}, inside, recess);  // z = 0
    if (!recess) mb.face({c[4], c[6], c[7], c[5]}, inside, false);  // z = sz: a recess's opening
}

/// Hip and gable roofs rise along z over the rectangle [0,sx] x [0,sy].
/// Written for a ridge along x; `along` swaps the roles of x and y.
void meshRoof(const Scope& s, bool hip, MeshBuilder& mb) {
    const bool alongX = s.size.x >= s.size.y;
    const float len = alongX ? s.size.x : s.size.y;   // along the ridge
    const float span = alongX ? s.size.y : s.size.x;  // across it
    const float h = s.size.z;
    auto at = [&](float along, float across, float up) {
        return alongX ? s.at(along, across, up) : s.at(across, along, up);
    };

    const uint32_t b0 = mb.point(at(0, 0, 0));
    const uint32_t b1 = mb.point(at(len, 0, 0));
    const uint32_t b2 = mb.point(at(len, span, 0));
    const uint32_t b3 = mb.point(at(0, span, 0));
    const Vec3 inside = at(len * 0.5f, span * 0.5f, h * 0.25f);

    const float inset = hip ? span * 0.5f : 0.0f;
    if (len - 2.0f * inset <= kEpsilon) {  // a square hip roof is a pyramid
        const uint32_t apex = mb.point(at(len * 0.5f, span * 0.5f, h));
        mb.face({b0, b1, apex}, inside, false);
        mb.face({b1, b2, apex}, inside, false);
        mb.face({b2, b3, apex}, inside, false);
        mb.face({b3, b0, apex}, inside, false);
        return;
    }
    const uint32_t r0 = mb.point(at(inset, span * 0.5f, h));
    const uint32_t r1 = mb.point(at(len - inset, span * 0.5f, h));
    mb.face({b0, b1, r1, r0}, inside, false);  // the two long slopes
    mb.face({b2, b3, r0, r1}, inside, false);
    mb.face({b3, b0, r0}, inside, false);      // hip slopes, or gable walls
    mb.face({b1, b2, r1}, inside, false);
}

void meshShape(const Shape& shape, MeshBuilder& mb) {
    const Scope& s = shape.scope;
    const int flat = (s.size.x <= kEpsilon) + (s.size.y <= kEpsilon) + (s.size.z <= kEpsilon);
    if (flat >= 2) return;  // a line or a point: nothing to see

    if (flat == 1) {
        // One quad. With (i, j, k) a cyclic order of the axes, cross(e_i, e_j)
        // == e_k, so walking i then j makes the quad face +k.
        const int k = flatAxis(s);
        const int i = (k + 1) % 3, j = (k + 2) % 3;
        const Vec3 u = s.axis(i) * s.size[i];
        const Vec3 v = s.axis(j) * s.size[j];
        const uint32_t a = mb.point(s.origin);
        const uint32_t b = mb.point(s.origin + u);
        const uint32_t c = mb.point(s.origin + u + v);
        const uint32_t d = mb.point(s.origin + v);
        mb.polygon({a, b, c, d});
        return;
    }

    switch (shape.asset) {
        case Asset::Box:    meshBox(s, false, mb); break;
        case Asset::Recess: meshBox(s, true, mb); break;
        case Asset::Hip:    meshRoof(s, true, mb); break;
        case Asset::Gable:  meshRoof(s, false, mb); break;
    }
}

}  // namespace

GeometryPtr mesh(const Geometry& shapes) {
    const ShapeView view(shapes);
    MeshBuilder mb;
    for (size_t i = 0; i < view.size(); ++i) {
        mb.owner = static_cast<uint32_t>(i);
        meshShape(view.shape(i), mb);
    }

    auto out = std::make_shared<Geometry>();
    out->detail() = shapes.detail();
    out->addPoints(mb.points.size());
    std::copy(mb.points.begin(), mb.points.end(), out->positionsForWrite().begin());
    size_t first = 0;
    for (uint32_t count : mb.counts) {
        out->addPrimitive(std::span<const uint32_t>(mb.corners.data() + first, count), true);
        first += count;
    }

    // Each polygon inherits its shape's attributes. The scope describes the
    // shape, not the polygon, so it stays behind.
    AttributeSet prims = shapes.points();
    prims.gather(mb.owners);
    for (const char* name : {"P", kAttrX, kAttrY, kAttrZ, kAttrSize}) prims.erase(name);
    out->primitives() = std::move(prims);
    return out;
}

}  // namespace pg::grammar
