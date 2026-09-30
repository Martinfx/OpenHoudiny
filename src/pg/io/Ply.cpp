#include "pg/io/Ply.h"

#include "pg/core/Instances.h"
#include "pg/io/Obj.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace pg::io {
namespace {

/// Little-endian bytes, whatever the machine is.
void put32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(v >> (8 * i)));
}
void putFloat(std::string& out, float v) {
    uint32_t b = 0;
    std::memcpy(&b, &v, sizeof b);
    put32(out, b);
}

/// A property of the vertices as written: an attribute's component, or a colour byte.
struct Column {
    std::string name;
    const AttributeArray* attribute = nullptr;
    int component = 0;
    bool colorByte = false;  ///< Cd, 0 to 1 as 0 to 255
    bool integer = false;
};

std::vector<Column> columnsOf(const Geometry& geo) {
    std::vector<Column> out;
    const AttributeSet& points = geo.points();
    auto components = [](AttrType t) {
        return t == AttrType::Vec2 ? 2 : t == AttrType::Vec3 ? 3 : t == AttrType::Vec4 ? 4 : 1;
    };
    auto add = [&](const std::string& attr, std::initializer_list<const char*> names, bool colour) {
        const AttributeArray* a = points.find(attr);
        if (!a || a->type() == AttrType::String || a->type() == AttrType::Int) return false;
        const int n = components(a->type());
        if (n < static_cast<int>(names.size()) - (colour ? 1 : 0)) return false;
        int c = 0;
        for (const char* name : names) {
            if (c >= n) break;
            out.push_back({name, a, c++, colour, false});
        }
        return true;
    };
    add("P", {"x", "y", "z"}, false);
    std::vector<std::string> done = {"P"};
    if (add("N", {"nx", "ny", "nz"}, false)) done.push_back("N");
    if (add("Cd", {"red", "green", "blue", "alpha"}, true)) done.push_back("Cd");
    if (add("v", {"vx", "vy", "vz"}, false)) done.push_back("v");
    static const char* parts[4] = {"_x", "_y", "_z", "_w"};
    for (const std::string& name : points.names()) {
        if (std::find(done.begin(), done.end(), name) != done.end()) continue;
        const AttributeArray* a = points.find(name);
        if (a->type() == AttrType::String) continue;
        const int n = components(a->type());
        for (int c = 0; c < n; ++c) {
            out.push_back({n == 1 ? name : name + parts[c], a, c, false, a->type() == AttrType::Int});
        }
    }
    return out;
}

float component(const AttributeArray& a, size_t i, int c) {
    switch (a.type()) {
        case AttrType::Float: return a.read<float>()[i];
        case AttrType::Int: return static_cast<float>(a.read<int32_t>()[i]);  // (written as ints: formatPly)
        case AttrType::Vec2: {
            const Vec2 v = a.read<Vec2>()[i];
            return c == 0 ? v.x : v.y;
        }
        case AttrType::Vec3: return a.read<Vec3>()[i][c];
        case AttrType::Vec4: {
            const Vec4 v = a.read<Vec4>()[i];
            const float x[4] = {v.x, v.y, v.z, v.w};
            return x[c];
        }
        case AttrType::String: return 0.0f;
    }
    return 0.0f;
}

// --- reading -----------------------------------------------------------------------------

enum class Scalar { I8, U8, I16, U16, I32, U32, F32, F64 };

bool scalarOf(const std::string& s, Scalar& out) {
    static const std::map<std::string, Scalar> names = {
        {"char", Scalar::I8},    {"int8", Scalar::I8},    {"uchar", Scalar::U8},   {"uint8", Scalar::U8},
        {"short", Scalar::I16},  {"int16", Scalar::I16},  {"ushort", Scalar::U16}, {"uint16", Scalar::U16},
        {"int", Scalar::I32},    {"int32", Scalar::I32},  {"uint", Scalar::U32},   {"uint32", Scalar::U32},
        {"float", Scalar::F32},  {"float32", Scalar::F32}, {"double", Scalar::F64}, {"float64", Scalar::F64}};
    const auto it = names.find(s);
    if (it == names.end()) return false;
    out = it->second;
    return true;
}

size_t sizeOf(Scalar s) {
    switch (s) {
        case Scalar::I8:
        case Scalar::U8: return 1;
        case Scalar::I16:
        case Scalar::U16: return 2;
        case Scalar::I32:
        case Scalar::U32:
        case Scalar::F32: return 4;
        case Scalar::F64: return 8;
    }
    return 4;
}

bool isInteger(Scalar s) { return s != Scalar::F32 && s != Scalar::F64; }

struct Property {
    std::string name;
    Scalar type = Scalar::F32;
    bool list = false;
    Scalar count = Scalar::U8;  ///< a list's length
};

struct Element {
    std::string name;
    size_t count = 0;
    std::vector<Property> properties;
};

/// Reads values one after another, from ASCII words or little-endian bytes.
class In {
public:
    In(std::string_view data, size_t at, bool binary) : data_(data), at_(at), binary_(binary) {}

    size_t left() const { return data_.size() - at_; }

    bool read(Scalar type, double& out) {
        if (!binary_) {
            const char* s = data_.data() + at_;
            const char* end = data_.data() + data_.size();
            while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) ++s;
            float v = 0.0f;
            if (!readNumber(s, end, v)) return false;
            at_ = static_cast<size_t>(s - data_.data());
            out = v;
            return true;
        }
        const size_t n = sizeOf(type);
        if (at_ + n > data_.size()) return false;
        uint64_t bits = 0;
        for (size_t i = 0; i < n; ++i) bits |= static_cast<uint64_t>(static_cast<unsigned char>(data_[at_ + i])) << (8 * i);
        at_ += n;
        switch (type) {
            case Scalar::I8: out = static_cast<int8_t>(bits); break;
            case Scalar::U8: out = static_cast<uint8_t>(bits); break;
            case Scalar::I16: out = static_cast<int16_t>(bits); break;
            case Scalar::U16: out = static_cast<uint16_t>(bits); break;
            case Scalar::I32: out = static_cast<int32_t>(bits); break;
            case Scalar::U32: out = static_cast<uint32_t>(bits); break;
            case Scalar::F32: {
                const uint32_t b = static_cast<uint32_t>(bits);
                float f = 0.0f;
                std::memcpy(&f, &b, sizeof f);
                out = f;
                break;
            }
            case Scalar::F64: {
                double d = 0.0;
                std::memcpy(&d, &bits, sizeof d);
                out = d;
                break;
            }
        }
        return true;
    }

private:
    std::string_view data_;
    size_t at_;
    bool binary_;
};

}  // namespace

std::string formatPly(const Geometry& geo) {
    // Instances as copies: what they stand for, where they stand.
    if (geo.prototypeCount() > 0) return formatPly(*unpackInstances(geo));
    const std::vector<Column> columns = columnsOf(geo);
    std::vector<size_t> faces;
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        if (geo.primitiveClosed(p) && geo.primitiveVertexCount(p) >= 3 && geo.primitiveVertexCount(p) <= 255) faces.push_back(p);
    }
    std::ostringstream header;
    header << "ply\nformat binary_little_endian 1.0\ncomment written by prototype\n";
    header << "element vertex " << geo.pointCount() << "\n";
    for (const Column& c : columns) {
        header << "property " << (c.colorByte ? "uchar" : c.integer ? "int" : "float") << ' ' << c.name << "\n";
    }
    if (!faces.empty()) header << "element face " << faces.size() << "\nproperty list uchar int vertex_indices\n";
    header << "end_header\n";
    std::string out = header.str();
    out.reserve(out.size() + geo.pointCount() * columns.size() * 4 + faces.size() * 17);
    for (size_t i = 0; i < geo.pointCount(); ++i) {
        for (const Column& c : columns) {
            if (c.integer) {
                put32(out, static_cast<uint32_t>(c.attribute->read<int32_t>()[i]));
                continue;
            }
            const float v = component(*c.attribute, i, c.component);
            if (c.colorByte) {
                const float byte = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) * 255.0f : 0.0f;
                out.push_back(static_cast<char>(static_cast<uint8_t>(std::lround(byte))));
            } else {
                putFloat(out, v);
            }
        }
    }
    for (const size_t p : faces) {
        const auto pts = geo.primitivePoints(p);
        out.push_back(static_cast<char>(static_cast<uint8_t>(pts.size())));
        for (const uint32_t q : pts) put32(out, q);
    }
    return out;
}

bool writePly(const Geometry& geo, const std::string& path, std::string& error) {
    const std::string bytes = formatPly(geo);
    std::ofstream file(path, std::ios::binary);
    if (!file || !file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        error = path + ": cannot write it";
        return false;
    }
    return true;
}

bool parsePly(std::string_view data, Geometry& geo, std::string& error) {
    // The header, line by line.
    if (data.substr(0, 3) != "ply") {
        error = "not a PLY file: it does not start with 'ply'";
        return false;
    }
    std::vector<Element> elements;
    bool binary = false;
    size_t at = 0;
    bool ended = false;
    while (at < data.size()) {
        const size_t end = std::min(data.find('\n', at), data.size());
        std::string line(data.substr(at, end - at));
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream words(line);
        std::string word;
        words >> word;
        if (word == "format") {
            std::string format;
            words >> format;
            if (format == "binary_little_endian") {
                binary = true;
            } else if (format != "ascii") {
                error = "a PLY file in " + format + ": only ascii and binary_little_endian are read";
                return false;
            }
        } else if (word == "element") {
            Element e;
            words >> e.name >> e.count;
            elements.push_back(e);
        } else if (word == "property") {
            if (elements.empty()) {
                error = "a PLY property before any element";
                return false;
            }
            Property p;
            std::string type;
            words >> type;
            if (type == "list") {
                std::string count, item;
                words >> count >> item >> p.name;
                p.list = true;
                if (!scalarOf(count, p.count) || !scalarOf(item, p.type)) {
                    error = "a PLY list of unknown types: " + line;
                    return false;
                }
            } else {
                words >> p.name;
                if (!scalarOf(type, p.type)) {
                    error = "a PLY property of unknown type: " + line;
                    return false;
                }
            }
            elements.back().properties.push_back(p);
        } else if (word == "end_header") {
            ended = true;
            break;
        }
    }
    if (!ended) {
        error = "the PLY header does not end (end_header)";
        return false;
    }

    geo = Geometry();
    In in(data, at, binary);
    auto cut = [&](const Element& e) {
        error = "the PLY file ends inside element " + e.name;
        return false;
    };
    for (const Element& e : elements) {
        const bool vertices = e.name == "vertex";
        const bool faces = e.name == "face";
        // The least an element takes -- in ASCII a byte a value -- bounds how
        // many the rest of the file holds: a count in the header that says
        // more is not believed.
        size_t least = 0;
        for (const Property& p : e.properties) least += binary ? sizeOf(p.list ? p.count : p.type) : 1;
        if (e.count > 0 && (least == 0 || e.count > in.left() / least)) {
            error = "the PLY file holds fewer '" + e.name + "' elements than its header says (" + std::to_string(e.count) + ")";
            return false;
        }
        std::vector<std::vector<double>> columns(vertices ? e.properties.size() : 0);
        for (auto& c : columns) c.reserve(e.count);
        std::vector<double> list;
        std::vector<uint32_t> corners;
        for (size_t i = 0; i < e.count; ++i) {
            for (size_t p = 0; p < e.properties.size(); ++p) {
                const Property& prop = e.properties[p];
                double v = 0.0;
                if (!prop.list) {
                    if (!in.read(prop.type, v)) return cut(e);
                    if (vertices) columns[p].push_back(v);
                    continue;
                }
                double count = 0.0;
                if (!in.read(prop.count, count)) return cut(e);
                if (!(count >= 0.0) || count > static_cast<double>(in.left())) return cut(e);
                list.clear();
                for (size_t k = 0; k < static_cast<size_t>(count); ++k) {
                    if (!in.read(prop.type, v)) return cut(e);
                    list.push_back(v);
                }
                if (!faces || (prop.name != "vertex_indices" && prop.name != "vertex_index")) continue;
                corners.clear();
                for (const double c : list) {
                    if (!(c >= 0.0 && c < static_cast<double>(geo.pointCount()))) {
                        error = "a PLY face names a vertex that is not there";
                        return false;
                    }
                    corners.push_back(static_cast<uint32_t>(c));
                }
                if (corners.size() >= 3) geo.addPrimitive(corners, true);
            }
        }
        if (!vertices) continue;
        // The columns, back into attributes.
        geo.addPoints(e.count);
        auto find = [&](const std::string& name) -> int {
            for (size_t p = 0; p < e.properties.size(); ++p) {
                if (!e.properties[p].list && e.properties[p].name == name) return static_cast<int>(p);
            }
            return -1;
        };
        std::vector<bool> used(e.properties.size(), false);
        // Properties `names` as the components of vector attribute `attr`.
        auto vector = [&](const std::string& attr, const std::vector<std::string>& names, float scale) {
            std::vector<size_t> idx;
            for (const std::string& n : names) {
                const int at = find(n);
                if (at < 0) return false;
                idx.push_back(static_cast<size_t>(at));
            }
            auto value = [&](size_t c, size_t i) { return static_cast<float>(columns[idx[c]][i]) * scale; };
            if (idx.size() == 4) {
                auto out = geo.points().create(attr, AttrType::Vec4).write<Vec4>();
                for (size_t i = 0; i < e.count; ++i) out[i] = Vec4(value(0, i), value(1, i), value(2, i), value(3, i));
            } else if (idx.size() == 2) {
                auto out = geo.points().create(attr, AttrType::Vec2).write<Vec2>();
                for (size_t i = 0; i < e.count; ++i) out[i] = Vec2(value(0, i), value(1, i));
            } else {
                auto out = attr == "P" ? geo.positionsForWrite() : geo.points().create(attr, AttrType::Vec3).write<Vec3>();
                for (size_t i = 0; i < e.count; ++i) out[i] = Vec3(value(0, i), value(1, i), value(2, i));
            }
            for (const size_t k : idx) used[k] = true;
            return true;
        };
        vector("P", {"x", "y", "z"}, 1.0f);
        vector("N", {"nx", "ny", "nz"}, 1.0f);
        // Colours as bytes, 0 to 255, or as numbers, 0 to 1; with alpha, four.
        const int red = find("red");
        const bool bytes = red >= 0 && isInteger(e.properties[static_cast<size_t>(red)].type);
        if (!vector("Cd", {"red", "green", "blue", "alpha"}, bytes ? 1.0f / 255.0f : 1.0f)) {
            vector("Cd", {"red", "green", "blue"}, bytes ? 1.0f / 255.0f : 1.0f);
        }
        vector("v", {"vx", "vy", "vz"}, 1.0f);
        // name_x name_y name_z: a vector; the rest: numbers.
        for (size_t p = 0; p < e.properties.size(); ++p) {
            const std::string& name = e.properties[p].name;
            if (used[p] || e.properties[p].list) continue;
            if (name.size() > 2 && name.compare(name.size() - 2, 2, "_x") == 0) {
                const std::string base = name.substr(0, name.size() - 2);
                if (vector(base, {base + "_x", base + "_y", base + "_z", base + "_w"}, 1.0f) ||
                    vector(base, {base + "_x", base + "_y", base + "_z"}, 1.0f) ||
                    vector(base, {base + "_x", base + "_y"}, 1.0f)) {
                    continue;
                }
            }
            if (isInteger(e.properties[p].type)) {
                auto out = geo.points().create(name, AttrType::Int).write<int32_t>();
                for (size_t i = 0; i < e.count; ++i) {
                    const double x = columns[p][i];
                    out[i] = std::isfinite(x) ? static_cast<int32_t>(std::clamp(x, -2147483648.0, 2147483647.0)) : 0;
                }
            } else {
                auto out = geo.points().create(name, AttrType::Float).write<float>();
                for (size_t i = 0; i < e.count; ++i) out[i] = static_cast<float>(columns[p][i]);
            }
            used[p] = true;
        }
    }
    return true;
}

bool readPly(const std::string& path, Geometry& geo, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = path + ": no such file";
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string data = ss.str();
    if (!parsePly(data, geo, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

}  // namespace pg::io
