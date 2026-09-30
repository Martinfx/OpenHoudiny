#include "pg/io/Usda.h"

#include "pg/core/Instances.h"

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
    out << (p.specifier.empty() ? "def" : p.specifier) << " ";
    if (!p.type.empty()) out << p.type << " ";
    out << quoted(p.name);
    if (!p.metadata.empty()) {
        out << " (\n";
        for (const std::string& m : p.metadata) {
            // Each line of it where it sits.
            for (size_t at = 0; at <= m.size();) {
                const size_t end = std::min(m.find('\n', at), m.size());
                indent(out, depth + 1);
                out.write(m.data() + at, static_cast<std::streamsize>(end - at));
                out << "\n";
                at = end + 1;
            }
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

std::string clips(const std::vector<std::pair<int, std::string>>& assets, const std::string& manifest,
                  const std::string& primPath) {
    std::string active, paths, times;
    for (size_t i = 0; i < assets.size(); ++i) {
        const std::string f = std::to_string(assets[i].first);
        const char* comma = i > 0 ? ", " : "";
        active += comma + ("(" + f + ", " + std::to_string(i) + ")");
        paths += comma + asset(assets[i].second);
        times += comma + ("(" + f + ", " + f + ")");
    }
    return "clips = {\n"
           "    dictionary default = {\n"
           "        double2[] active = [" + active + "]\n"
           "        asset[] assetPaths = [" + paths + "]\n"
           "        asset manifestAssetPath = " + asset(manifest) + "\n"
           "        string primPath = " + quoted(primPath) + "\n"
           "        double2[] times = [" + times + "]\n"
           "    }\n"
           "}";
}

std::vector<Field> pointPrimvars(const Geometry& geo, std::span<const uint32_t> source) {
    std::vector<Field> out;
    for (const std::string& name : geo.points().names()) {
        if (name == "P" || name == "N" || name == "v" || name == "Cd" || name == "pscale" || name == "id" ||
            name.rfind("__", 0) == 0) {
            continue;
        }
        const AttributeArray* a = geo.points().find(name);
        if (!a || a->size() != geo.pointCount()) continue;
        Field f;
        f.name = "primvars:" + identifier(name);
        f.metadata = interpolation("vertex");
        switch (a->type()) {
            case AttrType::Float: {
                std::vector<float> v;
                v.reserve(source.size());
                for (const uint32_t p : source) v.push_back(a->read<float>()[p]);
                f.type = "float[]";
                f.value = numbers(v);
                break;
            }
            case AttrType::Int: {
                std::vector<int32_t> v;
                v.reserve(source.size());
                for (const uint32_t p : source) v.push_back(a->read<int32_t>()[p]);
                f.type = "int[]";
                f.value = integers(v);
                break;
            }
            case AttrType::Vec3: {
                std::vector<Vec3> v;
                v.reserve(source.size());
                for (const uint32_t p : source) v.push_back(a->read<Vec3>()[p]);
                f.type = "float3[]";
                f.value = tuples(v);
                break;
            }
            default: continue;
        }
        out.push_back(std::move(f));
    }
    return out;
}


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
    // The normals and the velocities, where the points have them; the rest
    // as primvars.
    auto vectors = [&](const char* name) {
        const AttributeArray* a = geo.points().find(name);
        if (!a || a->type() != AttrType::Vec3 || a->size() != geo.pointCount()) return std::string();
        std::vector<Vec3> v;
        v.reserve(source.size());
        for (const uint32_t p : source) v.push_back(a->read<Vec3>()[p]);
        return tuples(v);
    };
    // Normals only where every point has one: a zero one -- merged in from
    // points that had them -- would shade it black; without, the renderer
    // makes its own, as the viewport does.
    m.normals = vectors("N");
    if (const AttributeArray* N = geo.points().find("N"); N && !m.normals.empty()) {
        for (const uint32_t p : source) {
            const Vec3& n = N->read<Vec3>()[p];
            if (dot(n, n) <= 1e-24f) {
                m.normals.clear();
                break;
            }
        }
    }
    m.velocities = vectors("v");
    m.primvars = pointPrimvars(geo, source);
    for (const uint32_t p : source) local[p] = -1;
    return m;
}

namespace {

/// The fields of frames of one prim, each animated: written once when alike.
/// The metadata of each is the first frame's that has it.
template <class Text>
void animateFields(Prim& prim, const std::vector<std::pair<int, Text>>& frames) {
    std::vector<std::string> order;
    std::vector<std::pair<std::string, std::string>> kinds;  // type and metadata, by name as in order
    std::vector<std::vector<std::pair<int, std::string>>> values;
    for (const auto& [f, text] : frames) {
        for (Field& field : fields(text)) {
            size_t i = static_cast<size_t>(std::find(order.begin(), order.end(), field.name) - order.begin());
            if (i == order.size()) {
                order.push_back(field.name);
                kinds.emplace_back(field.type, field.metadata);
                values.emplace_back();
            }
            values[i].emplace_back(f, std::move(field.value));
        }
    }
    std::vector<size_t> sorted(order.size());
    for (size_t i = 0; i < sorted.size(); ++i) sorted[i] = i;
    std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) { return order[a] < order[b]; });
    for (const size_t i : sorted) animate(prim, kinds[i].first, order[i], values[i], kinds[i].second);
}

}  // namespace

std::vector<Field> fields(const MeshText& m) {
    std::vector<Field> out;
    out.push_back({"float3[]", "extent", "", m.extent});
    out.push_back({"int[]", "faceVertexCounts", "", m.counts});
    out.push_back({"int[]", "faceVertexIndices", "", m.indices});
    if (!m.normals.empty()) out.push_back({"normal3f[]", "normals", interpolation("vertex"), m.normals});
    out.push_back({"point3f[]", "points", "", m.points});
    // The colour's interpolation is the first frame's: a colour of another
    // kind later is taken as the first frame's kind would read it.
    out.push_back({"color3f[]", "primvars:displayColor", m.colorHow, m.colors});
    for (const Field& f : m.primvars) out.push_back(f);
    if (!m.velocities.empty()) out.push_back({"vector3f[]", "velocities", "", m.velocities});
    std::sort(out.begin(), out.end(), [](const Field& a, const Field& b) { return a.name < b.name; });
    return out;
}

std::vector<Field> fields(const CurvesText& c) {
    return {{"int[]", "curveVertexCounts", "", c.counts},
            {"float3[]", "extent", "", c.extent},
            {"point3f[]", "points", "", c.points},
            {"color3f[]", "primvars:displayColor", interpolation("vertex"), c.colors}};
}

std::vector<Field> fields(const PointsText& p) {
    std::vector<Field> out;
    out.push_back({"float3[]", "extent", "", p.extent});
    if (!p.ids.empty()) out.push_back({"int64[]", "ids", "", p.ids});
    out.push_back({"point3f[]", "points", "", p.points});
    out.push_back({"color3f[]", "primvars:displayColor", interpolation("vertex"), p.colors});
    for (const Field& f : p.primvars) out.push_back(f);
    if (!p.velocities.empty()) out.push_back({"vector3f[]", "velocities", "", p.velocities});
    out.push_back({"float[]", "widths", interpolation("vertex"), p.widths});
    std::sort(out.begin(), out.end(), [](const Field& a, const Field& b) { return a.name < b.name; });
    return out;
}

Prim meshPrim(const std::string& name, const std::vector<std::pair<int, MeshText>>& frames) {
    Prim mesh("Mesh", name);
    animateFields(mesh, frames);
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
    std::vector<uint32_t> loose;
    for (size_t p = 0; p < used.size(); ++p) {
        if (!used[p]) loose.push_back(static_cast<uint32_t>(p));
    }
    t.primvars = pointPrimvars(geo, loose);
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
    animateFields(c, frames);
    c.setUniform("token", "type", quoted("linear"));
    c.set("float[]", "widths", "[0.01]").metadata = interpolation("constant");
    return c;
}

Prim pointsPrim(const std::string& name, const std::vector<std::pair<int, PointsText>>& frames) {
    Prim p("Points", name);
    animateFields(p, frames);
    return p;
}

InstancesText instancesText(const Geometry& geo) {
    std::vector<int32_t> indices, ids;
    std::vector<Vec3> positions, scales, tints;
    std::string orientations = "[";
    Bounds box;
    const AttributeArray* tint = geo.points().find("tint");
    const bool tinted = tint && tint->type() == AttrType::Vec3 && tint->size() == geo.pointCount();
    const AttributeArray* id = geo.points().find("id");
    const bool named = id && id->type() == AttrType::Int && id->size() == geo.pointCount();
    if (geo.prototypeCount() > 0) {
        // The prototype each point stands for, in the points' order.
        std::vector<int32_t> which(geo.pointCount(), -1);
        const auto byPrototype = instancesByPrototype(geo);
        for (size_t k = 0; k < byPrototype.size(); ++k) {
            for (const uint32_t p : byPrototype[k]) which[p] = static_cast<int32_t>(k);
        }
        const std::vector<Placement> places = placementsOf(geo);
        for (size_t p = 0; p < which.size(); ++p) {
            if (which[p] < 0) continue;
            indices.push_back(which[p]);
            positions.push_back(places[p].at);
            scales.push_back(Vec3(places[p].scale, places[p].scale, places[p].scale));
            if (indices.size() > 1) orientations += ", ";
            orientations += quat(places[p].orient);
            if (tinted) tints.push_back(tint->read<Vec3>()[p]);
            if (named) ids.push_back(id->read<int32_t>()[p]);
        }
        Vec3 lo, hi;
        instancesBox(geo, lo, hi);
        if (lo.x <= hi.x) {
            box.grow(lo);
            box.grow(hi);
        }
    }
    InstancesText t;
    t.indices = integers(indices);
    t.positions = tuples(positions);
    t.orientations = orientations + "]";
    t.scales = tuples(scales);
    if (tinted) t.tints = tuples(tints);
    if (named) t.ids = integers(ids);
    t.extent = box.extent();
    return t;
}

std::vector<Field> fields(const InstancesText& t) {
    std::vector<Field> out;
    out.push_back({"float3[]", "extent", "", t.extent});
    if (!t.ids.empty()) out.push_back({"int64[]", "ids", "", t.ids});
    out.push_back({"quath[]", "orientations", "", t.orientations});
    out.push_back({"point3f[]", "positions", "", t.positions});
    if (!t.tints.empty()) out.push_back({"color3f[]", "primvars:tint", interpolation("vertex"), t.tints});
    out.push_back({"int[]", "protoIndices", "", t.indices});
    out.push_back({"float3[]", "scales", "", t.scales});
    return out;
}

void addPrototypes(Prim& instancer, const std::string& path,
                   const std::vector<std::shared_ptr<const Geometry>>& prototypes) {
    Prim& scope = instancer.child("Scope", "Prototypes");
    std::string targets = "[";
    for (size_t k = 0; k < prototypes.size(); ++k) {
        const std::string name = "proto_" + std::to_string(k);
        const std::string at = path + "/Prototypes/" + name;
        const Geometry empty;
        scope.children.push_back(geometryPrim(name, {{0, prototypes[k] ? prototypes[k].get() : &empty}}, at));
        targets += (k > 0 ? ", <" : "<") + at + ">";
    }
    instancer.relate("prototypes", targets + "]");
}

Prim instancerPrim(const std::string& name, const std::string& path,
                   const std::vector<std::shared_ptr<const Geometry>>& prototypes,
                   const std::vector<std::pair<int, InstancesText>>& frames) {
    Prim p("PointInstancer", name);
    animateFields(p, frames);
    addPrototypes(p, path, prototypes);
    return p;
}

Prim geometryPrim(const std::string& name, const std::vector<std::pair<int, const Geometry*>>& frames,
                  const std::string& path) {
    const std::string at = path.empty() ? "/" + name : path;
    Prim g("Xform", name);
    std::vector<std::pair<int, MeshText>> meshes;
    std::vector<std::pair<int, CurvesText>> curves;
    std::vector<std::pair<int, PointsText>> points;
    std::vector<std::pair<int, InstancesText>> instances;
    const Geometry* prototypesOf = nullptr;  // the first frame with instances
    bool anyMesh = false, anyCurves = false, anyPoints = false;
    std::vector<int32_t> scratch;
    for (const auto& [f, geo] : frames) {
        // The instances apart; the rest as shapes.
        std::shared_ptr<Geometry> rest;
        const Geometry* shapes = geo;
        instances.emplace_back(f, instancesText(*geo));
        if (geo->prototypeCount() > 0) {
            rest = withoutInstances(*geo);
            shapes = rest.get();
            if (!prototypesOf && !instances.back().second.empty()) prototypesOf = geo;
        }
        std::vector<uint32_t> all(shapes->primitiveCount());
        for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<uint32_t>(i);
        meshes.emplace_back(f, meshText(*shapes, all, Vec3(), nullptr, scratch));
        curves.emplace_back(f, curvesText(*shapes));
        points.emplace_back(f, pointsText(*shapes));
        anyMesh = anyMesh || !meshes.back().second.empty();
        anyCurves = anyCurves || !curves.back().second.empty();
        anyPoints = anyPoints || !points.back().second.empty();
    }
    if (anyMesh) g.children.push_back(meshPrim("mesh", meshes));
    if (anyCurves) g.children.push_back(curvesPrim("curves", curves));
    if (anyPoints) g.children.push_back(pointsPrim("points", points));
    if (prototypesOf) g.children.push_back(instancerPrim("instances", at + "/instances", prototypesOf->prototypes(), instances));
    return g;
}

Stage geometryStage(const Geometry& geo, const std::string& name) {
    Stage s;
    const std::string prim = identifier(name.empty() ? "geometry" : name);
    s.metadata = {{"defaultPrim", quoted(prim)}, {"metersPerUnit", "1"}, {"upAxis", quoted("Y")}};
    s.prims.push_back(geometryPrim(prim, {{0, &geo}}, "/" + prim));
    return s;
}

}  // namespace pg::io::usda
