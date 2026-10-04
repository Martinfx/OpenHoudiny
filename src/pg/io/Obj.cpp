#include "pg/io/Obj.h"

#include "pg/core/Instances.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace pg::io {

bool readNumber(const char*& s, const char* end, float& out) {
    const char* p = s;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    bool negative = false;
    if (p < end && (*p == '-' || *p == '+')) negative = *p++ == '-';
    double value = 0.0;
    bool digits = false;
    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10.0 + (*p++ - '0');
        digits = true;
    }
    if (p < end && *p == '.') {
        ++p;
        double scale = 0.1;
        while (p < end && *p >= '0' && *p <= '9') {
            value += (*p++ - '0') * scale;
            scale *= 0.1;
            digits = true;
        }
    }
    if (!digits) return false;
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        bool negativeExponent = false;
        if (q < end && (*q == '-' || *q == '+')) negativeExponent = *q++ == '-';
        int exponent = 0;
        bool any = false;
        while (q < end && *q >= '0' && *q <= '9') {
            exponent = std::min(exponent * 10 + (*q++ - '0'), 400);
            any = true;
        }
        if (any) {
            value *= std::pow(10.0, negativeExponent ? -exponent : exponent);
            p = q;
        }
    }
    out = static_cast<float>(negative ? -value : value);
    s = p;
    return std::isfinite(out);
}

bool parseObj(std::string_view text, Geometry& out, std::string& error) {
    std::vector<Vec3> points;
    // The texture coordinates (vt), and of each corner which, -1 for none.
    std::vector<Vec2> uvs;
    std::vector<int64_t> cornerUv;
    struct Prim {
        std::vector<uint32_t> corners;
        bool closed;
    };
    std::vector<Prim> prims;
    size_t pos = 0;
    int line = 0;
    while (pos < text.size()) {
        const size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view l = text.substr(pos, end - pos);
        pos = end + 1;
        ++line;
        if (const size_t hash = l.find('#'); hash != std::string_view::npos) l = l.substr(0, hash);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.remove_suffix(1);
        while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
        if (l.size() > 3 && l[0] == 'v' && l[1] == 't' && (l[2] == ' ' || l[2] == '\t')) {
            // A texture coordinate: u and v -- a third, w, left out.
            const char* s = l.data() + 3;
            const char* e = l.data() + l.size();
            Vec2 t;
            if (!readNumber(s, e, t.x)) {
                error = "line " + std::to_string(line) + ": a texture coordinate is a number or two";
                return false;
            }
            if (!readNumber(s, e, t.y)) t.y = 0.0f;
            uvs.push_back(t);
            continue;
        }
        if (l.size() < 2 || (l[1] != ' ' && l[1] != '\t')) continue;  // vn, usemtl, o, g, s ...
        const char* s = l.data() + 2;
        const char* e = l.data() + l.size();
        if (l[0] == 'v') {
            Vec3 p;
            if (!readNumber(s, e, p.x) || !readNumber(s, e, p.y) || !readNumber(s, e, p.z)) {
                error = "line " + std::to_string(line) + ": a vertex is three numbers";
                return false;
            }
            points.push_back(p);
        } else if (l[0] == 'f' || l[0] == 'l') {
            Prim prim{{}, l[0] == 'f'};
            const long count = static_cast<long>(points.size());
            while (s < e) {
                while (s < e && (*s == ' ' || *s == '\t')) ++s;
                if (s >= e) break;
                // The point is the first number of a/b/c.
                char* stop = nullptr;
                const long ref = std::strtol(s, &stop, 10);
                if (stop == s) {
                    error = "line " + std::to_string(line) + ": a face lists vertex numbers";
                    return false;
                }
                const long index = ref < 0 ? count + ref : ref - 1;
                if (ref == 0 || index < 0 || index >= count) {
                    error = "line " + std::to_string(line) + ": no vertex " + std::to_string(ref);
                    return false;
                }
                prim.corners.push_back(static_cast<uint32_t>(index));
                s = stop;
                // a/b: b the texture coordinate's number, as a vertex's is --
                // none where there is no such coordinate.
                int64_t uv = -1;
                if (s < e && *s == '/' && s + 1 < e && s[1] != '/') {
                    const long t = std::strtol(s + 1, &stop, 10);
                    const long count2 = static_cast<long>(uvs.size());
                    const long at = t < 0 ? count2 + t : t - 1;
                    if (stop != s + 1 && t != 0 && at >= 0 && at < count2) uv = at;
                }
                cornerUv.push_back(uv);
                while (s < e && *s != ' ' && *s != '\t') ++s;  // the rest of a/b/c
            }
            // A face or a line too short is dropped -- its corners' coordinates too.
            if (prim.corners.size() < (prim.closed ? 3u : 2u)) cornerUv.resize(cornerUv.size() - prim.corners.size());
            if (prim.corners.size() >= (prim.closed ? 3u : 2u)) prims.push_back(std::move(prim));
        }
    }
    Geometry geo;
    geo.addPoints(points.size());
    std::copy(points.begin(), points.end(), geo.positionsForWrite().begin());
    for (const Prim& p : prims) geo.addPrimitive(p.corners, p.closed);
    // The corners' texture coordinates, where any has one: uv on the
    // vertices, (u, v, 0) -- 0 for a corner without.
    if (std::any_of(cornerUv.begin(), cornerUv.end(), [](int64_t i) { return i >= 0; }) &&
        cornerUv.size() == geo.vertexCount()) {
        auto uv = geo.vertices().create("uv", AttrType::Vec3).write<Vec3>();
        for (size_t v = 0; v < cornerUv.size(); ++v) {
            if (cornerUv[v] >= 0) uv[v] = Vec3(uvs[static_cast<size_t>(cornerUv[v])], 0.0f);
        }
    }
    out = std::move(geo);
    return true;
}

bool readObj(const std::string& path, Geometry& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": cannot read it";
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    if (!parseObj(text.str(), out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

std::string formatObj(const Geometry& geo) {
    // Instances as copies: what they stand for, where they stand.
    if (geo.prototypeCount() > 0) return formatObj(*unpackInstances(geo));
    std::string out = "# prototype\n";
    char buf[48];
    auto number = [&](float x) {
        const auto r = std::to_chars(buf, buf + sizeof buf, x);
        out.append(buf, r.ptr);
    };
    for (const Vec3& p : geo.positions()) {
        out += "v ";
        number(p.x);
        out += ' ';
        number(p.y);
        out += ' ';
        number(p.z);
        out += '\n';
    }
    // Texture coordinates: a vt for each corner (uv on the vertices) or for
    // each point (on the points), its number after the point's.
    auto uvOf = [](const AttributeArray* a) {
        return a && (a->type() == AttrType::Vec2 || a->type() == AttrType::Vec3) ? a : nullptr;
    };
    const AttributeArray* vertexUv = uvOf(geo.vertices().find("uv"));
    const AttributeArray* pointUv = vertexUv ? nullptr : uvOf(geo.points().find("uv"));
    if (const AttributeArray* uv = vertexUv ? vertexUv : pointUv) {
        for (size_t i = 0; i < uv->size(); ++i) {
            const Vec2 t = uv->type() == AttrType::Vec2 ? uv->read<Vec2>()[i] : Vec2(uv->read<Vec3>()[i]);
            out += "vt ";
            number(t.x);
            out += ' ';
            number(t.y);
            out += '\n';
        }
    }
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto pts = geo.primitivePoints(prim);
        const bool closed = geo.primitiveClosed(prim);
        if (pts.size() < (closed ? 3u : 2u)) continue;
        out += closed ? 'f' : 'l';
        const size_t first = geo.primitiveVertexStart(prim);
        for (size_t k = 0; k < pts.size(); ++k) {
            out += ' ' + std::to_string(pts[k] + 1);  // OBJ counts from 1
            if (vertexUv) out += '/' + std::to_string(first + k + 1);
            else if (pointUv) out += '/' + std::to_string(pts[k] + 1);
        }
        out += '\n';
    }
    return out;
}

bool writeObj(const Geometry& geo, const std::string& path, std::string& error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = path + ": cannot write it";
        return false;
    }
    out << formatObj(geo);
    if (!out) {
        error = path + ": writing it failed";
        return false;
    }
    return true;
}

}  // namespace pg::io
