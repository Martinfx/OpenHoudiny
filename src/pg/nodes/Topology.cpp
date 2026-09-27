// Nodes that change the mesh itself -- its pieces, its points, its faces:
// Connectivity, Fuse, PolyExtrude, Subdivide and Clip; and Attribute
// Transfer, which carries attributes from one geometry onto another by where
// their points are.
//
// A point these nodes make is a blend of points that were: the weights go
// with it (Blends), and every attribute of the points -- P, Cd, uv, N -- is
// blended by them alike: numbers weighted, integers and strings from the
// heaviest. So the nodes know nothing of the attributes they carry.
#include "pg/nodes/Nodes.h"

#include "pg/core/Parallel.h"
#include "pg/core/Spatial.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <sstream>
#include <unordered_map>

namespace pg {
namespace {

// --- blending -----------------------------------------------------------------------------------

/// New elements, each a weighted sum of old ones: the terms of element e are
/// index/weight[start[e], start[e + 1]).
struct Blends {
    std::vector<uint32_t> start{0};
    std::vector<uint32_t> index;
    std::vector<float> weight;

    size_t size() const { return start.size() - 1; }
    void one(uint32_t i) {
        index.push_back(i);
        weight.push_back(1.0f);
        start.push_back(static_cast<uint32_t>(index.size()));
    }
    /// (1 - t) a + t b.
    void two(uint32_t a, uint32_t b, float t) {
        std::vector<std::pair<uint32_t, float>> terms = {{a, 1.0f - t}, {b, t}};
        add(terms);
    }
    /// Terms naming the same element are summed.
    void add(std::vector<std::pair<uint32_t, float>>& terms) {
        std::sort(terms.begin(), terms.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        for (size_t i = 0; i < terms.size();) {
            const uint32_t at = terms[i].first;
            float w = 0.0f;
            while (i < terms.size() && terms[i].first == at) w += terms[i++].second;
            index.push_back(at);
            weight.push_back(w);
        }
        start.push_back(static_cast<uint32_t>(index.size()));
    }
    void none() { start.push_back(static_cast<uint32_t>(index.size())); }
    /// The term with the most weight, the first of equals; -1 for none.
    int64_t heaviest(size_t e) const {
        int64_t best = -1;
        float w = -1.0f;
        for (uint32_t k = start[e]; k < start[e + 1]; ++k) {
            if (weight[k] > w) {
                w = weight[k];
                best = index[k];
            }
        }
        return best;
    }
};

int floatsOf(AttrType t) {
    switch (t) {
        case AttrType::Float: return 1;
        case AttrType::Vec2: return 2;
        case AttrType::Vec3: return 3;
        case AttrType::Vec4: return 4;
        default: return 0;
    }
}

/// The attributes of `from` for elements blended from its own.
AttributeSet blended(const AttributeSet& from, const Blends& b) {
    AttributeSet out;
    out.setElementCount(b.size());
    std::vector<uint32_t> pick(b.size(), 0);
    for (size_t e = 0; e < b.size(); ++e) pick[e] = static_cast<uint32_t>(std::max<int64_t>(b.heaviest(e), 0));
    for (const std::string& name : from.names()) {
        const AttributeArray& a = *from.find(name);
        const int k = floatsOf(a.type());
        if (k == 0) {
            // Integers and strings: the heaviest term's, its string table shared.
            if (a.size() == 0) out.create(name, a.type());
            else out.assign(name, a.gather(pick));
            continue;
        }
        AttributeArray& o = out.create(name, a.type());
        const float* src = reinterpret_cast<const float*>(a.rawRead());
        float* dst = reinterpret_cast<float*>(o.rawWrite());
        for (size_t e = 0; e < b.size(); ++e) {
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (uint32_t t = b.start[e]; t < b.start[e + 1]; ++t) {
                if (b.index[t] >= a.size()) continue;
                const float* v = src + static_cast<size_t>(b.index[t]) * static_cast<size_t>(k);
                for (int c = 0; c < k; ++c) acc[c] += b.weight[t] * v[c];
            }
            for (int c = 0; c < k; ++c) dst[e * static_cast<size_t>(k) + static_cast<size_t>(c)] = acc[c];
        }
    }
    return out;
}

/// The attributes of `from` gathered: element e is from's `source[e]`.
AttributeSet gathered(const AttributeSet& from, std::span<const uint32_t> source) {
    AttributeSet out = from;
    if (from.elementCount() == 0) {
        out.setElementCount(source.size());
        return out;
    }
    out.gather(source);
    return out;
}

/// The groups of `src` onto `dst`, whose points are `points` blended from
/// src's (members where every term is one) and whose primitives are
/// `prims` gathered from src's.
void carryGroups(const Geometry& src, Geometry& dst, const Blends& points, std::span<const uint32_t> prims) {
    for (const std::string& name : src.groupNames()) {
        const Group* g = src.findGroup(name);
        if (g->classOf() == AttrClass::Point) {
            Group& o = dst.createGroup(name, AttrClass::Point);
            for (size_t e = 0; e < points.size(); ++e) {
                bool all = points.start[e + 1] > points.start[e];
                for (uint32_t t = points.start[e]; t < points.start[e + 1] && all; ++t) all = g->contains(points.index[t]);
                if (all) o.set(e, true);
            }
        } else if (g->classOf() == AttrClass::Primitive) {
            Group& o = dst.createGroup(name, AttrClass::Primitive);
            for (size_t e = 0; e < prims.size(); ++e) {
                if (g->contains(prims[e])) o.set(e, true);
            }
        }
    }
}

/// A geometry of `pointCount` points and the primitives `faces` (point
/// lists, `closed`), its attributes from `src`: points blended, vertices
/// blended, primitives gathered by `sourcePrim`; groups carried; detail and
/// volumes kept.
std::shared_ptr<Geometry> rebuild(const Geometry& src, const Blends& points, const std::vector<std::vector<uint32_t>>& faces,
                                  const std::vector<uint8_t>& closed, const Blends& vertices,
                                  const std::vector<uint32_t>& sourcePrim) {
    auto out = std::make_shared<Geometry>();
    out->addPoints(points.size());
    for (size_t f = 0; f < faces.size(); ++f) out->addPrimitive(faces[f], closed[f] != 0);
    out->points() = blended(src.points(), points);
    out->vertices() = blended(src.vertices(), vertices);
    out->primitives() = gathered(src.primitives(), sourcePrim);
    out->detail() = src.detail();
    carryGroups(src, *out, points, sourcePrim);
    for (const Volume& v : src.volumes()) out->addVolume(v);
    return out;
}

uint64_t edgeKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

/// The words of a list of attribute names: "Cd v" -> {"Cd", "v"}.
std::vector<std::string> words(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

// --- Connectivity ---------------------------------------------------------------------------------

/// Which piece each primitive (or point) is in: an integer attribute, the
/// pieces numbered from 0 in the order their first primitive comes -- what
/// a For-Each over pieces goes by.
class ConnectivityNode : public Node {
public:
    explicit ConnectivityNode(std::string name) : Node("connectivity", std::move(name)) {
        setInputCount(1);
        params_.setString("attribute", "class");
        params_.setInt("class", 0);  // 0: primitives, 1: points
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0]) return std::make_shared<Geometry>();
        auto geo = editableCopy(in[0]);
        const size_t np = geo->pointCount(), nprims = geo->primitiveCount();
        std::vector<uint32_t> parent(np);
        std::iota(parent.begin(), parent.end(), 0u);
        auto find = [&](uint32_t x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        for (size_t p = 0; p < nprims; ++p) {
            const auto corners = geo->primitivePoints(p);
            for (size_t i = 1; i < corners.size(); ++i) {
                const uint32_t a = find(corners[0]), b = find(corners[i]);
                if (a != b) parent[std::max(a, b)] = std::min(a, b);
            }
        }
        // Numbered as the primitives come; loose points after them.
        std::unordered_map<uint32_t, int32_t> number;
        std::vector<int32_t> primClass(nprims, -1);
        for (size_t p = 0; p < nprims; ++p) {
            const auto corners = geo->primitivePoints(p);
            if (corners.empty()) {
                primClass[p] = static_cast<int32_t>(number.size());
                number.emplace(~static_cast<uint32_t>(p), primClass[p]);  // a piece of its own
                continue;
            }
            const uint32_t root = find(corners[0]);
            const auto it = number.emplace(root, static_cast<int32_t>(number.size())).first;
            primClass[p] = it->second;
        }
        const std::string attr = params_.getString("attribute", "class");
        if (attr.empty()) return geo;
        if (std::clamp(params_.evalInt("class", ctx, 0), 0, 1) == 0) {
            auto out = geo->primitives().create(attr, AttrType::Int).write<int32_t>();
            std::copy(primClass.begin(), primClass.end(), out.begin());
            return geo;
        }
        std::vector<uint8_t> used(np, 0);
        for (size_t p = 0; p < nprims; ++p) {
            for (const uint32_t c : geo->primitivePoints(p)) used[c] = 1;
        }
        auto out = geo->points().create(attr, AttrType::Int).write<int32_t>();
        int32_t next = static_cast<int32_t>(number.size());
        for (size_t i = 0; i < np; ++i) {
            if (used[i]) out[i] = number.at(find(static_cast<uint32_t>(i)));
            else out[i] = next++;
        }
        return geo;
    }
};

// --- Fuse -----------------------------------------------------------------------------------------

/// Points nearer each other than Distance made one, at their middle; the
/// primitives follow, those that fold to nothing go.
class FuseNode : public Node {
public:
    explicit FuseNode(std::string name) : Node("fuse", std::move(name)) {
        setInputCount(1);
        params_.setFloat("distance", 0.001f);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0]) return std::make_shared<Geometry>();
        const Geometry& src = *in[0];
        const float distance = std::max(0.0f, params_.evalFloat("distance", ctx, 0.001f));
        const auto P = src.positions();
        const size_t np = P.size();
        // Each point joins the first one near it that leads a cluster.
        PointTree tree(P);
        std::vector<int32_t> lead(np, -1);
        std::vector<int32_t> found;
        for (size_t i = 0; i < np; ++i) {
            if (lead[i] >= 0) continue;
            lead[i] = static_cast<int32_t>(i);
            tree.near(P[i], distance, 0, found);
            for (const int32_t j : found) {
                if (lead[static_cast<size_t>(j)] < 0) lead[static_cast<size_t>(j)] = static_cast<int32_t>(i);
            }
        }
        std::vector<uint32_t> index(np, 0);
        Blends points;
        std::vector<std::vector<std::pair<uint32_t, float>>> members(np);
        for (size_t i = 0; i < np; ++i) members[static_cast<size_t>(lead[i])].push_back({static_cast<uint32_t>(i), 0.0f});
        for (size_t i = 0; i < np; ++i) {
            if (lead[i] != static_cast<int32_t>(i)) continue;
            index[i] = static_cast<uint32_t>(points.size());
            // The lead's attributes; its place, the cluster's middle (below).
            points.one(static_cast<uint32_t>(i));
        }
        for (size_t i = 0; i < np; ++i) index[i] = index[static_cast<size_t>(lead[i])];

        std::vector<std::vector<uint32_t>> faces;
        std::vector<uint8_t> closed;
        std::vector<uint32_t> sourcePrim;
        Blends vertices;
        for (size_t p = 0; p < src.primitiveCount(); ++p) {
            const auto corners = src.primitivePoints(p);
            const size_t v0 = src.primitiveVertexStart(p);
            const bool isClosed = src.primitiveClosed(p);
            std::vector<uint32_t> face;
            std::vector<uint32_t> verts;
            for (size_t c = 0; c < corners.size(); ++c) {
                const uint32_t q = index[corners[c]];
                if (!face.empty() && face.back() == q) continue;
                face.push_back(q);
                verts.push_back(static_cast<uint32_t>(v0 + c));
            }
            if (isClosed && face.size() > 1 && face.front() == face.back()) {
                face.pop_back();
                verts.pop_back();
            }
            if (face.size() < (isClosed ? 3u : 2u)) continue;
            faces.push_back(std::move(face));
            closed.push_back(isClosed ? 1 : 0);
            sourcePrim.push_back(static_cast<uint32_t>(p));
            for (const uint32_t v : verts) vertices.one(v);
        }
        auto out = rebuild(src, points, faces, closed, vertices, sourcePrim);
        // Each at its cluster's middle.
        auto outP = out->positionsForWrite();
        for (size_t i = 0; i < np; ++i) {
            if (lead[i] != static_cast<int32_t>(i)) continue;
            Vec3 sum;
            for (const auto& m : members[i]) sum += P[m.first];
            outP[index[i]] = sum * (1.0f / static_cast<float>(members[i].size()));
        }
        return out;
    }
};

// --- PolyExtrude ----------------------------------------------------------------------------------

/// Each face pushed out along its normal by Distance -- inset by Inset first
/// -- with a wall along each of its edges: windows cut in, a roof lifted.
/// The faces moved are in the group Front Group, the walls in Side Group.
class PolyExtrudeNode : public Node {
public:
    explicit PolyExtrudeNode(std::string name) : Node("polyextrude", std::move(name)) {
        setInputCount(1);
        params_.setFloat("distance", 0.2f);
        params_.setFloat("inset", 0.0f);
        params_.setString("group", "");
        params_.setBool("outputback", false);
        params_.setString("frontgroup", "extrudeFront");
        params_.setString("sidegroup", "extrudeSide");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0]) return std::make_shared<Geometry>();
        const Geometry& src = *in[0];
        const float distance = params_.evalFloat("distance", ctx, 0.2f);
        const float inset = params_.evalFloat("inset", ctx, 0.0f);
        const bool back = params_.evalBool("outputback", ctx, false);
        const std::string groupName = params_.getString("group");
        const Group* group = groupName.empty() ? nullptr : src.findGroup(groupName);
        error_.clear();
        if (!groupName.empty() && (!group || group->classOf() != AttrClass::Primitive)) {
            error_ = "no primitive group '" + groupName + "'";
            return in[0];
        }
        const auto P = src.positions();
        auto chosen = [&](size_t p) {
            return src.primitiveClosed(p) && src.primitiveVertexCount(p) >= 3 && (!group || group->contains(p));
        };

        Blends points, vertices;
        for (size_t i = 0; i < src.pointCount(); ++i) points.one(static_cast<uint32_t>(i));
        std::vector<Vec3> moved;  // where the new points go
        std::vector<std::vector<uint32_t>> faces;
        std::vector<uint8_t> closed;
        std::vector<uint32_t> sourcePrim;
        std::vector<uint8_t> role;  // 0 as it was, 1 front, 2 side
        // What is not extruded -- and, asked for, the faces extruded as backs.
        for (size_t p = 0; p < src.primitiveCount(); ++p) {
            const bool ext = chosen(p);
            if (ext && !back) continue;
            const auto corners = src.primitivePoints(p);
            const size_t v0 = src.primitiveVertexStart(p);
            std::vector<uint32_t> face(corners.begin(), corners.end());
            std::vector<uint32_t> verts(corners.size());
            std::iota(verts.begin(), verts.end(), static_cast<uint32_t>(v0));
            if (ext) {
                // The back of the solid faces the other way.
                std::reverse(face.begin(), face.end());
                std::reverse(verts.begin(), verts.end());
            }
            faces.push_back(std::move(face));
            closed.push_back(src.primitiveClosed(p) ? 1 : 0);
            sourcePrim.push_back(static_cast<uint32_t>(p));
            role.push_back(0);
            for (const uint32_t v : verts) vertices.one(v);
        }
        // The faces extruded: a front and a wall an edge.
        for (size_t p = 0; p < src.primitiveCount(); ++p) {
            if (!chosen(p)) continue;
            const auto corners = src.primitivePoints(p);
            const size_t k = corners.size();
            const size_t v0 = src.primitiveVertexStart(p);
            const Vec3 n = normalize(polygonNormal(src, corners));
            std::vector<uint32_t> front(k);
            for (size_t i = 0; i < k; ++i) {
                const Vec3 a = P[corners[(i + k - 1) % k]], b = P[corners[i]], c = P[corners[(i + 1) % k]];
                Vec3 offset = n * distance;
                if (inset != 0.0f) {
                    // Inward is left of the way round (anticlockwise about n):
                    // the miter moves each edge in by Inset.
                    const Vec3 dIn = normalize(b - a), dOut = normalize(c - b);
                    const Vec3 leftIn = cross(n, dIn), leftOut = cross(n, dOut);
                    const Vec3 bis = normalize(leftIn + leftOut);
                    const float cosHalf = std::max(dot(bis, leftIn), 0.2f);
                    offset += bis * (inset / cosHalf);
                }
                front[i] = static_cast<uint32_t>(points.size());
                points.one(corners[i]);
                moved.push_back(b + offset);
            }
            faces.push_back(front);
            closed.push_back(1);
            sourcePrim.push_back(static_cast<uint32_t>(p));
            role.push_back(1);
            for (size_t i = 0; i < k; ++i) vertices.one(static_cast<uint32_t>(v0 + i));
            for (size_t i = 0; i < k; ++i) {
                const size_t j = (i + 1) % k;
                faces.push_back({corners[i], corners[j], front[j], front[i]});
                closed.push_back(1);
                sourcePrim.push_back(static_cast<uint32_t>(p));
                role.push_back(2);
                vertices.one(static_cast<uint32_t>(v0 + i));
                vertices.one(static_cast<uint32_t>(v0 + j));
                vertices.one(static_cast<uint32_t>(v0 + j));
                vertices.one(static_cast<uint32_t>(v0 + i));
            }
        }
        auto out = rebuild(src, points, faces, closed, vertices, sourcePrim);
        auto outP = out->positionsForWrite();
        for (size_t i = 0; i < moved.size(); ++i) outP[src.pointCount() + i] = moved[i];
        // Normals that were are wrong now: drawn from the faces again.
        out->points().erase("N");
        out->vertices().erase("N");
        const std::string frontName = params_.getString("frontgroup"), sideName = params_.getString("sidegroup");
        if (!frontName.empty()) {
            Group& g = out->createGroup(frontName, AttrClass::Primitive);
            for (size_t f = 0; f < role.size(); ++f) g.set(f, role[f] == 1);
        }
        if (!sideName.empty()) {
            Group& g = out->createGroup(sideName, AttrClass::Primitive);
            for (size_t f = 0; f < role.size(); ++f) g.set(f, role[f] == 2);
        }
        return out;
    }

    std::string cookError() const override { return error_; }

private:
    std::string error_;
};

// --- Subdivide ------------------------------------------------------------------------------------

/// One step of Catmull-Clark: each face split into quads at its middle and
/// the middles of its edges, the points moved to smooth the surface. Edges
/// with one face -- the boundary -- stay where they were, as curves; open
/// primitives stay as they are.
std::shared_ptr<Geometry> catmullClark(const Geometry& src) {
    const size_t np = src.pointCount(), nprims = src.primitiveCount();
    auto isFace = [&](size_t p) { return src.primitiveClosed(p) && src.primitiveVertexCount(p) >= 3; };

    // The edges, as the faces meet them; the faces of each.
    std::unordered_map<uint64_t, uint32_t> edgeOf;
    std::vector<std::array<uint32_t, 2>> edgeEnds;
    std::vector<std::vector<uint32_t>> edgeFaces;
    std::vector<std::vector<uint32_t>> faceEdges(nprims);
    for (size_t p = 0; p < nprims; ++p) {
        if (!isFace(p)) continue;
        const auto c = src.primitivePoints(p);
        for (size_t i = 0; i < c.size(); ++i) {
            const uint32_t a = c[i], b = c[(i + 1) % c.size()];
            const auto [it, fresh] = edgeOf.emplace(edgeKey(a, b), static_cast<uint32_t>(edgeEnds.size()));
            if (fresh) {
                edgeEnds.push_back({std::min(a, b), std::max(a, b)});
                edgeFaces.emplace_back();
            }
            edgeFaces[it->second].push_back(static_cast<uint32_t>(p));
            faceEdges[p].push_back(it->second);
        }
    }
    const size_t ne = edgeEnds.size();
    // For each point: its faces, its edges.
    std::vector<std::vector<uint32_t>> pointFaces(np), pointEdges(np);
    for (size_t p = 0; p < nprims; ++p) {
        if (!isFace(p)) continue;
        for (const uint32_t c : src.primitivePoints(p)) pointFaces[c].push_back(static_cast<uint32_t>(p));
    }
    for (size_t e = 0; e < ne; ++e) {
        pointEdges[edgeEnds[e][0]].push_back(static_cast<uint32_t>(e));
        pointEdges[edgeEnds[e][1]].push_back(static_cast<uint32_t>(e));
    }
    auto faceTerms = [&](size_t f, float w, std::vector<std::pair<uint32_t, float>>& terms) {
        const auto c = src.primitivePoints(f);
        const float each = w / static_cast<float>(c.size());
        for (const uint32_t q : c) terms.push_back({q, each});
    };

    // The points: the old ones moved, then an edge's each, then a face's each.
    Blends points;
    std::vector<std::pair<uint32_t, float>> terms;
    for (size_t v = 0; v < np; ++v) {
        terms.clear();
        const auto& faces = pointFaces[v];
        const auto& edges = pointEdges[v];
        std::vector<uint32_t> hard;  // the other ends of the boundary (or non-manifold) edges
        for (const uint32_t e : edges) {
            if (edgeFaces[e].size() != 2) hard.push_back(edgeEnds[e][0] == v ? edgeEnds[e][1] : edgeEnds[e][0]);
        }
        if (faces.empty() || hard.size() > 2 || hard.size() == 1 || (hard.size() == 2 && faces.size() == 1)) {
            // Loose; where the boundary is odd; a corner of one face -- a
            // grid's -- stays where it is.
            points.one(static_cast<uint32_t>(v));
            continue;
        }
        if (hard.size() == 2) {
            // On the boundary: a curve through it.
            terms = {{static_cast<uint32_t>(v), 0.75f}, {hard[0], 0.125f}, {hard[1], 0.125f}};
            points.add(terms);
            continue;
        }
        // Inside: (F + 2R + (n - 3) v) / n.
        const float n = static_cast<float>(edges.size());
        terms.push_back({static_cast<uint32_t>(v), (n - 3.0f) / n});
        const float perFace = 1.0f / (n * static_cast<float>(faces.size()));
        for (const uint32_t f : faces) faceTerms(f, perFace, terms);
        const float perEdge = 1.0f / (n * static_cast<float>(edges.size()));  // 2 R / n: each midpoint 2/(n*n), a half each end
        for (const uint32_t e : edges) {
            terms.push_back({edgeEnds[e][0], perEdge});
            terms.push_back({edgeEnds[e][1], perEdge});
        }
        points.add(terms);
    }
    for (size_t e = 0; e < ne; ++e) {
        terms.clear();
        if (edgeFaces[e].size() == 2) {
            terms = {{edgeEnds[e][0], 0.25f}, {edgeEnds[e][1], 0.25f}};
            faceTerms(edgeFaces[e][0], 0.25f, terms);
            faceTerms(edgeFaces[e][1], 0.25f, terms);
        } else {
            terms = {{edgeEnds[e][0], 0.5f}, {edgeEnds[e][1], 0.5f}};
        }
        points.add(terms);
    }
    std::vector<uint32_t> facePoint(nprims, 0);
    for (size_t p = 0; p < nprims; ++p) {
        if (!isFace(p)) continue;
        terms.clear();
        faceTerms(p, 1.0f, terms);
        facePoint[p] = static_cast<uint32_t>(points.size());
        points.add(terms);
    }

    // The faces: a quad a corner, in the order the faces were.
    std::vector<std::vector<uint32_t>> faces;
    std::vector<uint8_t> closed;
    std::vector<uint32_t> sourcePrim;
    Blends vertices;
    for (size_t p = 0; p < nprims; ++p) {
        const auto c = src.primitivePoints(p);
        const uint32_t v0 = static_cast<uint32_t>(src.primitiveVertexStart(p));
        if (!isFace(p)) {
            faces.emplace_back(c.begin(), c.end());
            closed.push_back(src.primitiveClosed(p) ? 1 : 0);
            sourcePrim.push_back(static_cast<uint32_t>(p));
            for (size_t i = 0; i < c.size(); ++i) vertices.one(v0 + static_cast<uint32_t>(i));
            continue;
        }
        const size_t k = c.size();
        for (size_t i = 0; i < k; ++i) {
            const size_t prev = (i + k - 1) % k;
            const uint32_t eNext = static_cast<uint32_t>(np) + faceEdges[p][i];
            const uint32_t ePrev = static_cast<uint32_t>(np) + faceEdges[p][prev];
            faces.push_back({c[i], eNext, facePoint[p], ePrev});
            closed.push_back(1);
            sourcePrim.push_back(static_cast<uint32_t>(p));
            // The corners' own attributes (uv) bilinear over the face.
            vertices.one(v0 + static_cast<uint32_t>(i));
            vertices.two(v0 + static_cast<uint32_t>(i), v0 + static_cast<uint32_t>((i + 1) % k), 0.5f);
            std::vector<std::pair<uint32_t, float>> all;
            for (size_t j = 0; j < k; ++j) all.push_back({v0 + static_cast<uint32_t>(j), 1.0f / static_cast<float>(k)});
            vertices.add(all);
            vertices.two(v0 + static_cast<uint32_t>(prev), v0 + static_cast<uint32_t>(i), 0.5f);
        }
    }
    auto out = rebuild(src, points, faces, closed, vertices, sourcePrim);
    out->points().erase("N");
    out->vertices().erase("N");
    return out;
}

class SubdivideNode : public Node {
public:
    explicit SubdivideNode(std::string name) : Node("subdivide", std::move(name)) {
        setInputCount(1);
        params_.setInt("iterations", 1);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0]) return std::make_shared<Geometry>();
        GeometryPtr geo = in[0];
        const int steps = std::clamp(params_.evalInt("iterations", ctx, 1), 0, 6);
        for (int s = 0; s < steps; ++s) geo = catmullClark(*geo);
        return geo;
    }
};

// --- Clip -----------------------------------------------------------------------------------------

/// Triangles of the simple polygon `loop` (points of `P`, round `normal`
/// anticlockwise), by cutting off ears: indices into `loop`.
std::vector<std::array<size_t, 3>> earClip(std::span<const Vec3> P, const std::vector<uint32_t>& loop, const Vec3& normal) {
    std::vector<std::array<size_t, 3>> out;
    std::vector<size_t> left(loop.size());
    std::iota(left.begin(), left.end(), size_t{0});
    auto at = [&](size_t i) { return P[loop[left[i]]]; };
    auto convex = [&](const Vec3& a, const Vec3& b, const Vec3& c) { return dot(cross(b - a, c - b), normal) > 0.0f; };
    auto inside = [&](const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
        return dot(cross(b - a, p - a), normal) >= 0.0f && dot(cross(c - b, p - b), normal) >= 0.0f &&
               dot(cross(a - c, p - c), normal) >= 0.0f;
    };
    size_t guard = 0;
    while (left.size() > 3 && guard++ < loop.size() * loop.size()) {
        const size_t m = left.size();
        bool cut = false;
        for (size_t i = 0; i < m; ++i) {
            const size_t a = (i + m - 1) % m, c = (i + 1) % m;
            if (!convex(at(a), at(i), at(c))) continue;
            bool empty = true;
            for (size_t j = 0; j < m && empty; ++j) {
                if (j == a || j == i || j == c) continue;
                if (inside(at(j), at(a), at(i), at(c))) empty = false;
            }
            if (!empty) continue;
            out.push_back({left[a], left[i], left[c]});
            left.erase(left.begin() + static_cast<long>(i));
            cut = true;
            break;
        }
        if (!cut) break;  // not simple: what is left, a fan
    }
    for (size_t i = 1; i + 1 < left.size(); ++i) out.push_back({left[0], left[i], left[i + 1]});
    return out;
}

/// What is on one side of a plane kept; the faces cut are cut along it and,
/// with Cap, closed again with faces on the plane (in the group Cap Group).
class ClipNode : public Node {
public:
    explicit ClipNode(std::string name) : Node("clip", std::move(name)) {
        setInputCount(1);
        params_.setVec3("origin", Vec3(0.0f, 0.0f, 0.0f));
        params_.setVec3("dir", Vec3(0.0f, 1.0f, 0.0f));
        params_.setInt("keep", 0);  // 0: above -- the side Direction points to; 1: below
        params_.setBool("cap", true);
        params_.setString("capgroup", "cut");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0]) return std::make_shared<Geometry>();
        const Geometry& src = *in[0];
        const Vec3 origin = params_.evalVec3("origin", ctx);
        Vec3 dir = normalize(params_.evalVec3("dir", ctx, Vec3(0.0f, 1.0f, 0.0f)));
        if (length(dir) == 0.0f) dir = Vec3(0.0f, 1.0f, 0.0f);
        if (params_.evalInt("keep", ctx, 0) == 1) dir = -dir;
        const bool cap = params_.evalBool("cap", ctx, true);
        return clip(src, origin, dir, cap, params_.getString("capgroup", "cut"));
    }

    static std::shared_ptr<Geometry> clip(const Geometry& src, const Vec3& origin, const Vec3& dir, bool cap,
                                          const std::string& capGroup) {
        const auto P = src.positions();
        const size_t np = P.size();
        std::vector<float> d(np);
        for (size_t i = 0; i < np; ++i) d[i] = dot(P[i] - origin, dir);
        auto keep = [&](uint32_t i) { return d[i] >= 0.0f; };

        Blends points;
        std::vector<uint32_t> index(np, ~0u);
        for (size_t i = 0; i < np; ++i) {
            if (!keep(static_cast<uint32_t>(i))) continue;
            index[i] = static_cast<uint32_t>(points.size());
            points.one(static_cast<uint32_t>(i));
        }
        std::unordered_map<uint64_t, uint32_t> crossingOf;
        std::vector<Vec3> crossingAt;  // the cut points, after the kept ones
        const size_t kept = points.size();
        // Where edge a-b meets the plane: a point of either end on it, or a new one.
        auto crossing = [&](uint32_t a, uint32_t b) -> uint32_t {
            if (d[a] == 0.0f) return index[a];
            if (d[b] == 0.0f) return index[b];
            const auto it = crossingOf.find(edgeKey(a, b));
            if (it != crossingOf.end()) return it->second;
            const uint32_t lo = std::min(a, b), hi = std::max(a, b);
            const float t = d[lo] / (d[lo] - d[hi]);
            const uint32_t made = static_cast<uint32_t>(points.size());
            points.two(lo, hi, t);
            crossingAt.push_back(P[lo] + (P[hi] - P[lo]) * t);
            crossingOf.emplace(edgeKey(a, b), made);
            return made;
        };

        std::vector<std::vector<uint32_t>> faces;
        std::vector<uint8_t> closed;
        std::vector<uint32_t> sourcePrim;
        Blends vertices;
        std::vector<std::pair<uint32_t, uint32_t>> cut;  // along the plane: a face's way out to its way back in
        std::vector<uint32_t> cutPrim;
        for (size_t p = 0; p < src.primitiveCount(); ++p) {
            const auto c = src.primitivePoints(p);
            const uint32_t v0 = static_cast<uint32_t>(src.primitiveVertexStart(p));
            const size_t k = c.size();
            if (k == 0) continue;
            const bool isClosed = src.primitiveClosed(p);
            if (!isClosed) {
                // A curve: the runs of it that are kept.
                std::vector<uint32_t> run, runVerts;
                std::vector<std::pair<uint32_t, float>> runT;  // vertex blends: (other vertex, t) per corner
                auto emit = [&] {
                    if (run.size() >= 2) {
                        faces.push_back(run);
                        closed.push_back(0);
                        sourcePrim.push_back(static_cast<uint32_t>(p));
                        for (size_t i = 0; i < run.size(); ++i) {
                            if (runT[i].first == ~0u) vertices.one(runVerts[i]);
                            else vertices.two(runVerts[i], runT[i].first, runT[i].second);
                        }
                    }
                    run.clear();
                    runVerts.clear();
                    runT.clear();
                };
                for (size_t i = 0; i < k; ++i) {
                    if (keep(c[i])) {
                        run.push_back(index[c[i]]);
                        runVerts.push_back(v0 + static_cast<uint32_t>(i));
                        runT.push_back({~0u, 0.0f});
                    }
                    if (i + 1 < k && keep(c[i]) != keep(c[i + 1])) {
                        const uint32_t x = crossing(c[i], c[i + 1]);
                        if (run.empty() || run.back() != x) {
                            run.push_back(x);
                            runVerts.push_back(v0 + static_cast<uint32_t>(i));
                            runT.push_back({v0 + static_cast<uint32_t>(i + 1), d[c[i]] / (d[c[i]] - d[c[i + 1]])});
                        }
                        if (keep(c[i])) emit();  // it leaves: the run ends
                    }
                }
                emit();
                continue;
            }
            // A face: Sutherland-Hodgman against the one plane.
            std::vector<uint32_t> face;
            std::vector<std::pair<uint32_t, std::pair<uint32_t, float>>> verts;  // vertex, (other vertex, t)
            std::vector<std::pair<uint32_t, bool>> crossings;  // the cut points, and whether the face leaves there
            auto push = [&](uint32_t q, uint32_t va, uint32_t vb, float t) {
                if (!face.empty() && face.back() == q) return;
                face.push_back(q);
                verts.push_back({va, {vb, t}});
            };
            for (size_t i = 0; i < k; ++i) {
                const uint32_t a = c[i], b = c[(i + 1) % k];
                const uint32_t va = v0 + static_cast<uint32_t>(i), vb = v0 + static_cast<uint32_t>((i + 1) % k);
                if (keep(a)) push(index[a], va, ~0u, 0.0f);
                if (keep(a) != keep(b)) {
                    const uint32_t x = crossing(a, b);
                    push(x, va, vb, d[a] / (d[a] - d[b]));
                    crossings.push_back({x, keep(a)});
                }
            }
            if (face.size() > 1 && face.front() == face.back()) {
                face.pop_back();
                verts.pop_back();
            }
            if (face.size() < 3) continue;
            faces.push_back(face);
            closed.push_back(1);
            sourcePrim.push_back(static_cast<uint32_t>(p));
            for (const auto& [va, other] : verts) {
                if (other.first == ~0u) vertices.one(va);
                else vertices.two(va, other.first, other.second);
            }
            // Along the plane, from where it leaves to where it comes back.
            for (size_t i = 0; i < crossings.size(); ++i) {
                if (!crossings[i].second) continue;
                for (size_t j = 1; j < crossings.size(); ++j) {
                    const auto& next = crossings[(i + j) % crossings.size()];
                    if (next.second) break;
                    if (next.first != crossings[i].first) {
                        cut.push_back({crossings[i].first, next.first});
                        cutPrim.push_back(static_cast<uint32_t>(p));
                    }
                    break;
                }
            }
        }

        // The caps: the cuts joined into loops, round the other way.
        std::vector<uint8_t> isCap(faces.size(), 0);
        if (cap && !cut.empty()) {
            std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> next;  // entry -> exit, and the face
            for (size_t i = 0; i < cut.size(); ++i) next.emplace(cut[i].second, std::make_pair(cut[i].first, cutPrim[i]));
            std::vector<uint32_t> starts;
            for (const auto& [from, to] : next) starts.push_back(from);
            std::sort(starts.begin(), starts.end());
            std::unordered_map<uint32_t, uint8_t> seen;
            // The places of all the points, kept and made, for the ear clipping.
            std::vector<Vec3> at(points.size());
            for (size_t i = 0; i < np; ++i) {
                if (index[i] != ~0u) at[index[i]] = P[i];
            }
            for (size_t i = 0; i < crossingAt.size(); ++i) at[kept + i] = crossingAt[i];
            for (const uint32_t s : starts) {
                if (seen.count(s)) continue;
                std::vector<uint32_t> loop;
                uint32_t prim = next.at(s).second;
                uint32_t q = s;
                bool whole = false;
                while (!seen.count(q)) {
                    seen.emplace(q, 1);
                    loop.push_back(q);
                    const auto it = next.find(q);
                    if (it == next.end()) break;
                    q = it->second.first;
                    if (q == s) {
                        whole = true;
                        break;
                    }
                }
                if (!whole || loop.size() < 3) continue;
                // Facing the side cut away.
                Vec3 n;
                for (size_t i = 0; i < loop.size(); ++i) {
                    const Vec3& a = at[loop[i]];
                    const Vec3& b = at[loop[(i + 1) % loop.size()]];
                    n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
                }
                if (dot(n, dir) > 0.0f) std::reverse(loop.begin(), loop.end());
                const auto tris = earClip(at, loop, -dir);
                const bool convexLoop = tris.size() == loop.size() - 2 && [&] {
                    for (size_t i = 0; i < loop.size(); ++i) {
                        const Vec3& a = at[loop[(i + loop.size() - 1) % loop.size()]];
                        const Vec3& b = at[loop[i]];
                        const Vec3& c = at[loop[(i + 1) % loop.size()]];
                        if (dot(cross(b - a, c - b), -dir) < 0.0f) return false;
                    }
                    return true;
                }();
                auto addCap = [&](std::vector<uint32_t> corners) {
                    faces.push_back(std::move(corners));
                    closed.push_back(1);
                    sourcePrim.push_back(prim);
                    isCap.push_back(1);
                    for (size_t i = 0; i < faces.back().size(); ++i) vertices.none();
                };
                if (convexLoop) {
                    addCap(loop);
                } else {
                    for (const auto& t : tris) addCap({loop[t[0]], loop[t[1]], loop[t[2]]});
                }
            }
        }
        isCap.resize(faces.size(), 0);
        auto out = rebuild(src, points, faces, closed, vertices, sourcePrim);
        if (!capGroup.empty() && cap) {
            // Added to: the caps of earlier cuts stay in it.
            Group& g = out->createGroup(capGroup, AttrClass::Primitive);
            for (size_t f = 0; f < isCap.size(); ++f) {
                if (isCap[f]) g.set(f, true);
            }
        }
        return out;
    }
};

// --- Attribute Transfer ---------------------------------------------------------------------------

/// Point attributes of the second input onto the points of the first near
/// them: within Distance, the weighted mean of the points there; beyond it,
/// fading out over Blend Width.
class AttribTransferNode : public Node {
public:
    explicit AttribTransferNode(std::string name) : Node("attribtransfer", std::move(name)) {
        setInputCount(2);
        params_.setString("attributes", "Cd");
        params_.setFloat("distance", 0.5f);
        params_.setFloat("blend", 0.0f);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0]) return std::make_shared<Geometry>();
        if (in.size() < 2 || !in[1] || in[1]->pointCount() == 0) return in[0];
        const Geometry& from = *in[1];
        auto geo = editableCopy(in[0]);
        const float distance = std::max(0.0f, params_.evalFloat("distance", ctx, 0.5f));
        const float blend = std::max(0.0f, params_.evalFloat("blend", ctx, 0.0f));
        std::vector<std::string> names = words(params_.getString("attributes", "Cd"));
        if (std::find(names.begin(), names.end(), "*") != names.end()) {
            names.clear();
            for (const std::string& n : from.points().names()) {
                if (n != "P") names.push_back(n);
            }
        }
        PointTree tree(from.positions());
        const auto P = geo->positions();
        const size_t n = P.size();
        // Who gives each point what, and how much of it.
        std::vector<std::vector<std::pair<int32_t, float>>> terms(n);
        std::vector<float> amount(n, 0.0f);
        std::vector<int32_t> nearest(n, -1);
        const auto src = from.positions();
        parallelFor(n, 1024, [&](size_t begin, size_t end) {
            std::vector<int32_t> found;
            for (size_t i = begin; i < end; ++i) {
                tree.near(P[i], distance + blend, 0, found);
                if (found.empty()) continue;
                nearest[i] = found.front();
                const float dmin = length(src[static_cast<size_t>(found.front())] - P[i]);
                if (dmin <= distance) {
                    amount[i] = 1.0f;
                    float total = 0.0f;
                    for (const int32_t j : found) {
                        const float dj = length(src[static_cast<size_t>(j)] - P[i]);
                        if (dj > distance) break;
                        const float w = distance > 0.0f ? std::max(1.0f - (dj / distance) * (dj / distance), 1e-4f) : 1.0f;
                        terms[i].push_back({j, w});
                        total += w;
                    }
                    for (auto& t : terms[i]) t.second /= total;
                } else if (blend > 0.0f) {
                    amount[i] = std::clamp(1.0f - (dmin - distance) / blend, 0.0f, 1.0f);
                    terms[i].push_back({found.front(), 1.0f});
                }
            }
        });
        error_.clear();
        for (const std::string& name : names) {
            const AttributeArray* a = from.points().find(name);
            if (!a || name == "P") {
                if (!a) error_ += (error_.empty() ? "" : "; ") + std::string("no point attribute '") + name + "' to transfer";
                continue;
            }
            const int k = floatsOf(a->type());
            AttributeArray* o = geo->points().find(name);
            if (!o || o->type() != a->type()) o = &geo->points().create(name, a->type());
            if (k == 0) {
                // Integers and strings: the nearest point's, where it takes over.
                std::vector<uint32_t> pick(n);
                const AttributeArray old = *o;
                for (size_t i = 0; i < n; ++i) pick[i] = static_cast<uint32_t>(std::max(nearest[i], 0));
                AttributeArray moved = a->gather(pick);
                if (a->type() == AttrType::String) {
                    // One table: the strings of those it keeps into it.
                    auto ids = moved.write<int32_t>();
                    const auto was = old.read<int32_t>();
                    for (size_t i = 0; i < n; ++i) {
                        if (nearest[i] < 0 || amount[i] < 0.5f) ids[i] = moved.internString(old.stringValue(was[i]));
                    }
                } else {
                    auto v = moved.write<int32_t>();
                    const auto was = old.read<int32_t>();
                    for (size_t i = 0; i < n; ++i) {
                        if (nearest[i] < 0 || amount[i] < 0.5f) v[i] = was[i];
                    }
                }
                geo->points().assign(name, moved);
                continue;
            }
            const float* s = reinterpret_cast<const float*>(a->rawRead());
            float* dst = reinterpret_cast<float*>(o->rawWrite());
            for (size_t i = 0; i < n; ++i) {
                if (terms[i].empty() || amount[i] <= 0.0f) continue;
                float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (const auto& [j, w] : terms[i]) {
                    for (int c = 0; c < k; ++c) acc[c] += w * s[static_cast<size_t>(j) * static_cast<size_t>(k) + static_cast<size_t>(c)];
                }
                for (int c = 0; c < k; ++c) {
                    float& v = dst[i * static_cast<size_t>(k) + static_cast<size_t>(c)];
                    v += (acc[c] - v) * amount[i];
                }
            }
        }
        return geo;
    }

    std::string cookError() const override { return error_; }

private:
    std::string error_;
};

}  // namespace

std::shared_ptr<Geometry> clipGeometry(const Geometry& src, const Vec3& origin, const Vec3& dir, bool cap,
                                       const std::string& capGroup) {
    return ClipNode::clip(src, origin, normalize(dir), cap, capGroup);
}

std::shared_ptr<Geometry> subdivideGeometry(const Geometry& src, int iterations) {
    std::shared_ptr<Geometry> geo = std::make_shared<Geometry>(src);
    for (int s = 0; s < iterations; ++s) geo = catmullClark(*geo);
    return geo;
}

void registerTopologyNodes() {
    auto& r = NodeRegistry::instance();
    r.add("connectivity", [](const std::string& n) { return std::make_unique<ConnectivityNode>(n); });
    r.add("fuse", [](const std::string& n) { return std::make_unique<FuseNode>(n); });
    r.add("polyextrude", [](const std::string& n) { return std::make_unique<PolyExtrudeNode>(n); });
    r.add("subdivide", [](const std::string& n) { return std::make_unique<SubdivideNode>(n); });
    r.add("clip", [](const std::string& n) { return std::make_unique<ClipNode>(n); });
    r.add("attribtransfer", [](const std::string& n) { return std::make_unique<AttribTransferNode>(n); });
}

}  // namespace pg
