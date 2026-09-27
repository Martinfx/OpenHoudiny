#include "pg/io/Usda.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <ostream>
#include <sstream>

namespace pg::io::usda {

namespace {

void appendNumber(std::string& out, float x) {
    if (!std::isfinite(x)) x = 0.0f;
    if (x == 0.0f) x = 0.0f;  // no "-0"
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, x);
    out.append(buf, r.ptr);
}

void appendTuple(std::string& out, const Vec3& v) {
    out += '(';
    appendNumber(out, v.x);
    out += ", ";
    appendNumber(out, v.y);
    out += ", ";
    appendNumber(out, v.z);
    out += ')';
}

void indent(std::ostream& out, int depth) {
    for (int i = 0; i < depth; ++i) out << "    ";
}

void writeAttribute(std::ostream& out, const Attribute& a, int depth) {
    const std::string head = (a.uniform ? "uniform " : "") + a.type + " " + a.name;
    // The declaration: its default and its metadata -- or the metadata
    // alone, or nothing at all when it has neither but has samples.
    const bool declared = !a.value.empty() || !a.metadata.empty() || a.samples.empty();
    if (declared) {
        indent(out, depth);
        out << head;
        if (!a.value.empty()) out << " = " << a.value;
        if (!a.metadata.empty()) {
            out << " (\n";
            indent(out, depth + 1);
            out << a.metadata << "\n";
            indent(out, depth);
            out << ")";
        }
        out << "\n";
    }
    if (!a.samples.empty()) {
        indent(out, depth);
        out << head << ".timeSamples = {\n";
        for (const auto& [time, value] : a.samples) {
            indent(out, depth + 1);
            std::string t;
            appendNumber(t, static_cast<float>(time));
            out << t << ": " << value << ",\n";
        }
        indent(out, depth);
        out << "}\n";
    }
}

void writePrim(std::ostream& out, const Prim& p, int depth) {
    indent(out, depth);
    out << "def ";
    if (!p.type.empty()) out << p.type << " ";
    out << quoted(p.name);
    if (!p.metadata.empty()) {
        out << " (\n";
        for (const std::string& m : p.metadata) {
            indent(out, depth + 1);
            out << m << "\n";
        }
        indent(out, depth);
        out << ")";
    }
    out << "\n";
    indent(out, depth);
    out << "{\n";
    for (const Attribute& a : p.attributes) writeAttribute(out, a, depth + 1);
    for (const auto& [name, target] : p.relationships) {
        indent(out, depth + 1);
        out << "rel " << name << " = " << target << "\n";
    }
    bool first = p.attributes.empty() && p.relationships.empty();
    for (const Prim& child : p.children) {
        if (!first) out << "\n";
        first = false;
        writePrim(out, child, depth + 1);
    }
    indent(out, depth);
    out << "}\n";
}

}  // namespace

std::string number(float x) {
    std::string out;
    appendNumber(out, x);
    return out;
}

std::string tuple(const Vec3& v) {
    std::string out;
    appendTuple(out, v);
    return out;
}

std::string quat(const Vec4& q) {
    std::string out = "(";
    appendNumber(out, q.w);
    for (const float c : {q.x, q.y, q.z}) {
        out += ", ";
        appendNumber(out, c);
    }
    return out + ")";
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

std::string tuples(std::span<const Vec3> v) {
    std::string out;
    out.reserve(v.size() * 30 + 2);
    out += '[';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ", ";
        appendTuple(out, v[i]);
    }
    return out + "]";
}

std::string numbers(std::span<const float> v) {
    std::string out;
    out.reserve(v.size() * 10 + 2);
    out += '[';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ", ";
        appendNumber(out, v[i]);
    }
    return out + "]";
}

std::string integers(std::span<const int32_t> v) {
    std::string out;
    out.reserve(v.size() * 6 + 2);
    out += '[';
    char buf[16];
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ", ";
        const auto r = std::to_chars(buf, buf + sizeof buf, v[i]);
        out.append(buf, r.ptr);
    }
    return out + "]";
}

std::string tokens(const std::vector<std::string>& v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ", ";
        out += quoted(v[i]);
    }
    return out + "]";
}

std::string asset(const std::string& path) { return "@" + path + "@"; }

std::string identifier(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9')) out.insert(out.begin(), '_');
    return out;
}

Attribute& Prim::set(const std::string& type, const std::string& name, std::string value) {
    Attribute a;
    a.type = type;
    a.name = name;
    a.value = std::move(value);
    attributes.push_back(std::move(a));
    return attributes.back();
}

Attribute& Prim::setUniform(const std::string& type, const std::string& name, std::string value) {
    Attribute& a = set(type, name, std::move(value));
    a.uniform = true;
    return a;
}

Prim& Prim::child(std::string type, std::string name) {
    children.emplace_back(std::move(type), std::move(name));
    return children.back();
}

void Stage::write(std::ostream& out) const {
    out << "#usda 1.0\n";
    if (!metadata.empty()) {
        out << "(\n";
        for (const auto& [name, value] : metadata) out << "    " << name << " = " << value << "\n";
        out << ")\n";
    }
    for (const Prim& p : prims) {
        out << "\n";
        writePrim(out, p, 0);
    }
}

std::string Stage::text() const {
    std::ostringstream out;
    write(out);
    return out.str();
}

bool writeStage(const Stage& stage, const std::string& path, std::string& error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    stage.write(out);
    out.flush();
    if (!out) {
        error = "cannot write " + path + " -- is the disk full?";
        return false;
    }
    return true;
}

namespace {

const Vec3 kGrey(0.72f, 0.72f, 0.74f);  // what the viewport draws what has no colour in

bool colorOf(const AttributeSet& set, size_t i, Vec3& out) {
    const AttributeArray* a = set.find("Cd");
    if (!a || i >= a->size()) return false;
    switch (a->type()) {
        case AttrType::Vec3: out = a->read<Vec3>()[i]; return true;
        case AttrType::Vec4: {
            const Vec4 c = a->read<Vec4>()[i];
            out = Vec3(c.x, c.y, c.z);
            return true;
        }
        case AttrType::Float: out = Vec3(a->read<float>()[i]); return true;
        default: return false;
    }
}

}  // namespace

void animate(Prim& p, const std::string& type, const std::string& name,
             const std::vector<std::pair<int, std::string>>& values, const std::string& metadata) {
    if (values.empty()) return;
    Attribute a;
    a.type = type;
    a.name = name;
    a.metadata = metadata;
    const bool alike = std::all_of(values.begin(), values.end(), [&](const auto& v) { return v.second == values[0].second; });
    if (alike) {
        a.value = values[0].second;
    } else {
        for (const auto& [frame, value] : values) a.samples.emplace_back(frame, value);
    }
    p.attributes.push_back(std::move(a));
}

std::string interpolation(const char* how) { return std::string("interpolation = \"") + how + "\""; }


MeshText meshText(const Geometry& geo, std::span<const uint32_t> prims, const Vec3& middle, const Group* inside,
                std::vector<int32_t>& local) {
    const auto P = geo.positions();
    if (local.size() < geo.pointCount()) local.assign(geo.pointCount(), -1);  // left all -1 by each call
    std::vector<Vec3> points, corners;
    std::vector<uint32_t> source;
    std::vector<int32_t> counts, indices;
    MeshText m;
    Vec3 detail = kGrey;
    colorOf(geo.detail(), 0, detail);
    Bounds box;
    for (const uint32_t prim : prims) {
        if (!geo.primitiveClosed(prim)) continue;
        const auto pts = geo.primitivePoints(prim);
        if (pts.size() < 3) continue;
        const size_t start = geo.primitiveVertexStart(prim);
        if (inside && inside->contains(prim)) m.inside.push_back(static_cast<int32_t>(counts.size()));
        counts.push_back(static_cast<int32_t>(pts.size()));
        for (size_t k = 0; k < pts.size(); ++k) {
            const uint32_t p = pts[k];
            if (local[p] < 0) {
                local[p] = static_cast<int32_t>(points.size());
                points.push_back(P[p] - middle);
                source.push_back(p);
                box.grow(points.back());
            }
            indices.push_back(local[p]);
            Vec3 c;
            if (!(colorOf(geo.vertices(), start + k, c) || colorOf(geo.points(), p, c) ||
                  colorOf(geo.primitives(), prim, c))) {
                c = detail;
            }
            corners.push_back(c);
        }
    }
    m.points = tuples(points);
    m.counts = integers(counts);
    m.indices = integers(indices);
    m.extent = box.extent();
    // The colour: one when all corners have it, a face each when each face
    // is of one, else a corner each.
    bool one = true, perFace = true;
    std::vector<Vec3> faces;
    size_t at = 0;
    for (const int32_t n : counts) {
        for (int32_t k = 1; k < n; ++k) perFace = perFace && corners[at + static_cast<size_t>(k)] == corners[at];
        faces.push_back(corners[at]);
        at += static_cast<size_t>(n);
    }
    for (const Vec3& c : corners) one = one && c == corners.front();
    if (corners.empty() || one) {
        const Vec3 c = corners.empty() ? detail : corners.front();
        m.colors = tuples(std::span<const Vec3>(&c, 1));
        m.colorHow = interpolation("constant");
    } else if (perFace) {
        m.colors = tuples(faces);
        m.colorHow = interpolation("uniform");
    } else {
        m.colors = tuples(corners);
        m.colorHow = interpolation("faceVarying");
    }
    // The normals, where the points have them.
    const AttributeArray* N = geo.points().find("N");
    if (N && N->type() == AttrType::Vec3 && N->size() == geo.pointCount()) {
        std::vector<Vec3> normals;
        normals.reserve(source.size());
        for (const uint32_t p : source) normals.push_back(N->read<Vec3>()[p]);
        m.normals = tuples(normals);
    }
    for (const uint32_t p : source) local[p] = -1;
    return m;
}

Prim meshPrim(const std::string& name, const std::vector<std::pair<int, MeshText>>& frames) {
    Prim mesh("Mesh", name);
    auto each = [&](auto field) {
        std::vector<std::pair<int, std::string>> v;
        v.reserve(frames.size());
        for (const auto& [f, m] : frames) v.emplace_back(f, field(m));
        return v;
    };
    animate(mesh, "float3[]", "extent", each([](const MeshText& m) { return m.extent; }));
    animate(mesh, "int[]", "faceVertexCounts", each([](const MeshText& m) { return m.counts; }));
    animate(mesh, "int[]", "faceVertexIndices", each([](const MeshText& m) { return m.indices; }));
    if (!frames.empty() && !frames[0].second.normals.empty()) {
        animate(mesh, "normal3f[]", "normals", each([](const MeshText& m) { return m.normals; }), interpolation("vertex"));
    }
    animate(mesh, "point3f[]", "points", each([](const MeshText& m) { return m.points; }));
    // The colour's interpolation is the first frame's: a colour of another
    // kind later is taken as the first frame's kind would read it.
    if (!frames.empty()) {
        animate(mesh, "color3f[]", "primvars:displayColor", each([](const MeshText& m) { return m.colors; }),
                frames[0].second.colorHow);
    }
    mesh.setUniform("token", "subdivisionScheme", quoted("none"));
    return mesh;
}


CurvesText curvesText(const Geometry& geo) {
    const auto P = geo.positions();
    std::vector<Vec3> points, colors;
    std::vector<int32_t> counts;
    Vec3 detail = kGrey;
    colorOf(geo.detail(), 0, detail);
    Bounds box;
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        if (geo.primitiveClosed(prim)) continue;
        const auto pts = geo.primitivePoints(prim);
        if (pts.size() < 2) continue;
        const size_t start = geo.primitiveVertexStart(prim);
        counts.push_back(static_cast<int32_t>(pts.size()));
        for (size_t k = 0; k < pts.size(); ++k) {
            points.push_back(P[pts[k]]);
            box.grow(P[pts[k]]);
            Vec3 c;
            if (!(colorOf(geo.vertices(), start + k, c) || colorOf(geo.points(), pts[k], c) ||
                  colorOf(geo.primitives(), prim, c))) {
                c = detail;
            }
            colors.push_back(c);
        }
    }
    CurvesText t;
    t.points = tuples(points);
    t.counts = integers(counts);
    t.colors = tuples(colors);
    t.extent = box.extent(0.005f);
    return t;
}


PointsText pointsText(const Geometry& geo) {
    std::vector<uint8_t> used(geo.pointCount(), 0);
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        for (const uint32_t p : geo.primitivePoints(prim)) {
            if (p < used.size()) used[p] = 1;
        }
    }
    const auto P = geo.positions();
    const AttributeArray* pscale = geo.points().find("pscale");
    const bool sized = pscale && pscale->type() == AttrType::Float && pscale->size() == geo.pointCount();
    const AttributeArray* v = geo.points().find("v");
    const bool moving = v && v->type() == AttrType::Vec3 && v->size() == geo.pointCount();
    const AttributeArray* id = geo.points().find("id");
    const bool named = id && id->type() == AttrType::Int && id->size() == geo.pointCount();
    Vec3 detail = kGrey;
    colorOf(geo.detail(), 0, detail);
    std::vector<Vec3> points, colors, velocities;
    std::vector<float> widths;
    std::vector<int32_t> ids;
    Bounds box;
    float widest = 0.0f;
    for (size_t p = 0; p < used.size(); ++p) {
        if (used[p]) continue;
        points.push_back(P[p]);
        box.grow(P[p]);
        widths.push_back(sized ? 2.0f * std::max(pscale->read<float>()[p], 0.0f) : 0.02f);
        widest = std::max(widest, widths.back());
        Vec3 c;
        if (!colorOf(geo.points(), p, c)) c = detail;
        colors.push_back(c);
        if (moving) velocities.push_back(v->read<Vec3>()[p]);
        if (named) ids.push_back(id->read<int32_t>()[p]);
    }
    PointsText t;
    t.points = tuples(points);
    t.widths = numbers(widths);
    t.colors = tuples(colors);
    if (moving) t.velocities = tuples(velocities);
    if (named) t.ids = integers(ids);
    t.extent = box.extent(0.5f * widest);
    return t;
}

void Bounds::grow(const Vec3& p) {
    for (int a = 0; a < 3; ++a) {
        lo[a] = std::min(lo[a], p[a]);
        hi[a] = std::max(hi[a], p[a]);
    }
}

void Bounds::grow(const Bounds& b) {
    if (b.empty()) return;
    grow(b.lo);
    grow(b.hi);
}

std::string Bounds::extent(float pad) const {
    const Vec3 l = empty() ? Vec3() : lo - Vec3(pad), h = empty() ? Vec3() : hi + Vec3(pad);
    return "[" + tuple(l) + ", " + tuple(h) + "]";
}

Prim curvesPrim(const std::string& name, const std::vector<std::pair<int, CurvesText>>& frames) {
    Prim c("BasisCurves", name);
    auto each = [&](auto field) {
        std::vector<std::pair<int, std::string>> v;
        for (const auto& [f, t] : frames) v.emplace_back(f, field(t));
        return v;
    };
    animate(c, "int[]", "curveVertexCounts", each([](const CurvesText& t) { return t.counts; }));
    animate(c, "float3[]", "extent", each([](const CurvesText& t) { return t.extent; }));
    animate(c, "point3f[]", "points", each([](const CurvesText& t) { return t.points; }));
    animate(c, "color3f[]", "primvars:displayColor", each([](const CurvesText& t) { return t.colors; }),
            interpolation("vertex"));
    c.setUniform("token", "type", quoted("linear"));
    c.set("float[]", "widths", "[0.01]").metadata = interpolation("constant");
    return c;
}

Prim pointsPrim(const std::string& name, const std::vector<std::pair<int, PointsText>>& frames) {
    Prim p("Points", name);
    auto each = [&](auto field) {
        std::vector<std::pair<int, std::string>> v;
        for (const auto& [f, t] : frames) v.emplace_back(f, field(t));
        return v;
    };
    animate(p, "float3[]", "extent", each([](const PointsText& t) { return t.extent; }));
    if (!frames.empty() && !frames[0].second.ids.empty()) {
        animate(p, "int64[]", "ids", each([](const PointsText& t) { return t.ids; }));
    }
    animate(p, "point3f[]", "points", each([](const PointsText& t) { return t.points; }));
    animate(p, "color3f[]", "primvars:displayColor", each([](const PointsText& t) { return t.colors; }),
            interpolation("vertex"));
    if (!frames.empty() && !frames[0].second.velocities.empty()) {
        animate(p, "vector3f[]", "velocities", each([](const PointsText& t) { return t.velocities; }));
    }
    animate(p, "float[]", "widths", each([](const PointsText& t) { return t.widths; }), interpolation("vertex"));
    return p;
}

Prim geometryPrim(const std::string& name, const std::vector<std::pair<int, const Geometry*>>& frames) {
    Prim g("Xform", name);
    std::vector<std::pair<int, MeshText>> meshes;
    std::vector<std::pair<int, CurvesText>> curves;
    std::vector<std::pair<int, PointsText>> points;
    bool anyMesh = false, anyCurves = false, anyPoints = false;
    std::vector<int32_t> scratch;
    for (const auto& [f, geo] : frames) {
        std::vector<uint32_t> all(geo->primitiveCount());
        for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<uint32_t>(i);
        meshes.emplace_back(f, meshText(*geo, all, Vec3(), nullptr, scratch));
        curves.emplace_back(f, curvesText(*geo));
        points.emplace_back(f, pointsText(*geo));
        anyMesh = anyMesh || !meshes.back().second.empty();
        anyCurves = anyCurves || !curves.back().second.empty();
        anyPoints = anyPoints || !points.back().second.empty();
    }
    if (anyMesh) g.children.push_back(meshPrim("mesh", meshes));
    if (anyCurves) g.children.push_back(curvesPrim("curves", curves));
    if (anyPoints) g.children.push_back(pointsPrim("points", points));
    return g;
}

Stage geometryStage(const Geometry& geo, const std::string& name) {
    Stage s;
    const std::string prim = identifier(name.empty() ? "geometry" : name);
    s.metadata = {{"defaultPrim", quoted(prim)}, {"metersPerUnit", "1"}, {"upAxis", quoted("Y")}};
    s.prims.push_back(geometryPrim(prim, {{0, &geo}}));
    return s;
}

}  // namespace pg::io::usda
