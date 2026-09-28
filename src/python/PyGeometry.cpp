// pg.Geometry: a geometry's attributes, topology, groups and volumes as
// arrays without a copy; building one from Python; files in and out.
#include "Bindings.h"

#include "pg/io/Export.h"
#include "pg/io/Obj.h"
#include "pg/io/Ply.h"

#include <pybind11/stl.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace pg::python {

namespace {

const char* typeName(AttrType t) {
    switch (t) {
        case AttrType::Int: return "int";
        case AttrType::Float: return "float";
        case AttrType::Vec2: return "vector2";
        case AttrType::Vec3: return "vector3";
        case AttrType::Vec4: return "vector4";
        case AttrType::String: return "string";
    }
    return "float";
}

AttrType typeOf(const std::string& name) {
    if (name == "int") return AttrType::Int;
    if (name == "float") return AttrType::Float;
    if (name == "vector2") return AttrType::Vec2;
    if (name == "vector3" || name == "vector") return AttrType::Vec3;
    if (name == "vector4") return AttrType::Vec4;
    if (name == "string") return AttrType::String;
    throw Error("no attribute type '" + name + "': int, float, vector2, vector3, vector4 or string");
}

size_t widthOf(AttrType t) {
    switch (t) {
        case AttrType::Vec2: return 2;
        case AttrType::Vec3: return 3;
        case AttrType::Vec4: return 4;
        default: return 1;
    }
}

/// Numbers from Python, `width` a row: a buffer (a numpy array, a
/// memoryview) of one or two dimensions, or a sequence of numbers or of
/// rows. Whether they were all whole numbers, in `whole`.
struct Numbers {
    std::vector<double> values;
    size_t rows = 0, width = 1;
    bool whole = true;
};

double readItem(const char* at, char kind, size_t size) {
    switch (kind) {
        case 'f': return size == 4 ? static_cast<double>(*reinterpret_cast<const float*>(at))
                                   : *reinterpret_cast<const double*>(at);
        case 'd': return *reinterpret_cast<const double*>(at);
        case '?': return *reinterpret_cast<const bool*>(at) ? 1.0 : 0.0;
        case 'b': return static_cast<double>(*reinterpret_cast<const int8_t*>(at));
        case 'B': return static_cast<double>(*reinterpret_cast<const uint8_t*>(at));
        case 'h': return static_cast<double>(*reinterpret_cast<const int16_t*>(at));
        case 'H': return static_cast<double>(*reinterpret_cast<const uint16_t*>(at));
        default: break;
    }
    const bool isSigned = kind == 'i' || kind == 'l' || kind == 'q' || kind == 'n';
    if (size == 4) {
        return isSigned ? static_cast<double>(*reinterpret_cast<const int32_t*>(at))
                        : static_cast<double>(*reinterpret_cast<const uint32_t*>(at));
    }
    if (size == 8) {
        return isSigned ? static_cast<double>(*reinterpret_cast<const int64_t*>(at))
                        : static_cast<double>(*reinterpret_cast<const uint64_t*>(at));
    }
    throw Error(std::string("numbers of the kind '") + kind + "' are not read");
}

Numbers numbersOf(py::handle data, size_t width) {
    Numbers n;
    n.width = width;
    if (PyObject_CheckBuffer(data.ptr()) && !py::isinstance<py::str>(data) && !py::isinstance<py::bytes>(data)) {
        const py::buffer_info info = py::reinterpret_borrow<py::buffer>(data).request();
        std::string format = info.format;
        while (!format.empty() && std::string("@=<>!").find(format[0]) != std::string::npos) format.erase(0, 1);
        if (format.size() != 1) throw Error("numbers of the format '" + info.format + "' are not read");
        const char kind = format[0];
        if (kind == 'e') throw Error("half floats are not read: give float32 (numpy: .astype('float32'))");
        n.whole = kind != 'f' && kind != 'd';
        const size_t size = static_cast<size_t>(info.itemsize);
        if (info.ndim == 1 && width == 1) {
            n.rows = static_cast<size_t>(info.shape[0]);
        } else if (info.ndim == 2 && static_cast<size_t>(info.shape[1]) == width) {
            n.rows = static_cast<size_t>(info.shape[0]);
        } else if (info.ndim == 1 && static_cast<size_t>(info.shape[0]) % width == 0) {
            n.rows = static_cast<size_t>(info.shape[0]) / width;  // flat: x, y, z, x, y, z...
        } else {
            throw Error("wanted rows of " + std::to_string(width) + " numbers, not an array of that shape");
        }
        n.values.reserve(n.rows * width);
        const char* base = static_cast<const char*>(info.ptr);
        for (size_t r = 0; r < n.rows; ++r) {
            for (size_t c = 0; c < width; ++c) {
                const char* at = base;
                if (info.ndim == 2) {
                    at += static_cast<py::ssize_t>(r) * info.strides[0] + static_cast<py::ssize_t>(c) * info.strides[1];
                } else {
                    at += static_cast<py::ssize_t>(r * width + c) * info.strides[0];
                }
                n.values.push_back(readItem(at, kind, size));
            }
        }
        return n;
    }
    if (!py::isinstance<py::sequence>(data) || py::isinstance<py::str>(data)) {
        throw Error("wanted numbers: a numpy array, a memoryview or a list");
    }
    const py::sequence seq = py::reinterpret_borrow<py::sequence>(data);
    auto take = [&](py::handle x) {
        if (!py::isinstance<py::int_>(x) && !py::isinstance<py::float_>(x) && !py::isinstance<py::bool_>(x)) {
            // A numpy scalar and the like: whatever turns into a float.
            if (!PyNumber_Check(x.ptr())) throw Error("wanted a number, not " + py::str(py::type::of(x)).cast<std::string>());
        }
        const double v = py::cast<double>(py::float_(py::reinterpret_borrow<py::object>(x)));
        n.whole = n.whole && (py::isinstance<py::int_>(x) || py::isinstance<py::bool_>(x));
        n.values.push_back(v);
    };
    if (width == 1) {
        for (py::handle x : seq) take(x);
        n.rows = n.values.size();
        return n;
    }
    for (py::handle row : seq) {
        if (!py::isinstance<py::sequence>(row) || py::len(row) != width) {
            throw Error("wanted rows of " + std::to_string(width) + " numbers");
        }
        for (py::handle x : py::reinterpret_borrow<py::sequence>(row)) take(x);
        ++n.rows;
    }
    return n;
}

/// The rows of `data` as vectors, `width` numbers each.
std::vector<Vec3> vectorsOf(py::handle data) {
    const Numbers n = numbersOf(data, 3);
    std::vector<Vec3> out(n.rows);
    for (size_t i = 0; i < n.rows; ++i) {
        out[i] = Vec3(static_cast<float>(n.values[3 * i]), static_cast<float>(n.values[3 * i + 1]),
                      static_cast<float>(n.values[3 * i + 2]));
    }
    return out;
}

std::vector<uint32_t> indicesOf(py::handle data, size_t below, const char* what) {
    const Numbers n = numbersOf(data, 1);
    std::vector<uint32_t> out(n.rows);
    for (size_t i = 0; i < n.rows; ++i) {
        const double v = n.values[i];
        if (!(v >= 0.0) || v != std::floor(v) || v >= static_cast<double>(below)) {
            throw Error(std::string(what) + " " + std::to_string(v) + " is not one of the " + std::to_string(below));
        }
        out[i] = static_cast<uint32_t>(v);
    }
    return out;
}

/// The attribute as Python reads it: an array over its memory, or a list
/// of its strings.
py::object attributeOf(const PyGeometry& g, AttrClass cls, const std::string& name) {
    std::shared_ptr<const Geometry> snap = g.snapshot();
    const AttributeArray* a = snap->attributes(cls).find(name);
    if (!a) return py::none();
    switch (a->type()) {
        case AttrType::Int: {
            const auto v = a->read<int32_t>();
            return py::cast(rows(snap, v.data(), v.size(), 1));
        }
        case AttrType::Float: {
            const auto v = a->read<float>();
            return py::cast(rows(snap, v.data(), v.size(), 1));
        }
        case AttrType::Vec2: {
            const auto v = a->read<Vec2>();
            return py::cast(rows(snap, reinterpret_cast<const float*>(v.data()), v.size(), 2));
        }
        case AttrType::Vec3: {
            const auto v = a->read<Vec3>();
            return py::cast(rows(snap, reinterpret_cast<const float*>(v.data()), v.size(), 3));
        }
        case AttrType::Vec4: {
            const auto v = a->read<Vec4>();
            return py::cast(rows(snap, reinterpret_cast<const float*>(v.data()), v.size(), 4));
        }
        case AttrType::String: {
            py::list out;
            for (const int32_t index : a->read<int32_t>()) out.append(a->stringValue(index));
            return out;
        }
    }
    return py::none();
}

void setAttribute(PyGeometry& g, AttrClass cls, const std::string& name, py::handle data, const std::string& type) {
    Geometry& geo = g.edit();
    AttributeSet& set = geo.attributes(cls);
    const size_t count = cls == AttrClass::Detail ? 1 : set.elementCount();
    if (name.empty()) throw Error("an attribute needs a name");
    // Strings: a list of str (or one, for the detail).
    const bool text = type == "string" || py::isinstance<py::str>(data) ||
                      (type.empty() && py::isinstance<py::sequence>(data) && !PyObject_CheckBuffer(data.ptr()) &&
                       py::len(data) > 0 && py::isinstance<py::str>(py::reinterpret_borrow<py::sequence>(data)[0]));
    if (text) {
        std::vector<std::string> values;
        if (py::isinstance<py::str>(data)) {
            values.assign(count, data.cast<std::string>());
        } else {
            for (py::handle x : data) values.push_back(py::str(x).cast<std::string>());
        }
        if (values.size() != count) {
            throw Error(name + ": " + std::to_string(values.size()) + " values for " + std::to_string(count) + " elements");
        }
        AttributeArray& a = set.create(name, AttrType::String);
        std::vector<int32_t> index(count);
        for (size_t i = 0; i < count; ++i) index[i] = a.internString(values[i]);
        std::copy(index.begin(), index.end(), a.write<int32_t>().begin());
        return;
    }
    // Numbers: the width from the type, or from the data.
    AttrType t = type.empty() ? AttrType::Float : typeOf(type);
    size_t width = widthOf(t);
    if (type.empty()) {
        if (PyObject_CheckBuffer(data.ptr())) {
            const py::buffer_info info = py::reinterpret_borrow<py::buffer>(data).request();
            width = info.ndim == 2 ? static_cast<size_t>(info.shape[1]) : 1;
        } else if (py::isinstance<py::sequence>(data) && py::len(data) > 0 &&
                   py::isinstance<py::sequence>(py::reinterpret_borrow<py::sequence>(data)[0])) {
            width = py::len(py::reinterpret_borrow<py::sequence>(data)[0]);
        } else if (!py::isinstance<py::sequence>(data)) {
            width = 1;  // one number, for every element
        }
        if (width < 1 || width > 4) throw Error(name + ": rows of 1 to 4 numbers, not " + std::to_string(width));
    }
    Numbers n;
    if (!py::isinstance<py::sequence>(data) && !PyObject_CheckBuffer(data.ptr())) {
        n.width = 1;
        n.rows = count;
        n.values.assign(count, py::cast<double>(py::float_(py::reinterpret_borrow<py::object>(data))));
        n.whole = py::isinstance<py::int_>(data);
    } else {
        n = numbersOf(data, width);
    }
    if (type.empty()) {
        t = width == 2 ? AttrType::Vec2 : width == 3 ? AttrType::Vec3 : width == 4 ? AttrType::Vec4
            : n.whole ? AttrType::Int : AttrType::Float;
    }
    if (n.rows != count) {
        throw Error(name + ": " + std::to_string(n.rows) + " values for " + std::to_string(count) + " elements");
    }
    AttributeArray& a = set.create(name, t);
    if (t == AttrType::Int) {
        auto w = a.write<int32_t>();
        for (size_t i = 0; i < count; ++i) w[i] = static_cast<int32_t>(std::llround(n.values[i]));
        return;
    }
    float* w = t == AttrType::Float   ? a.write<float>().data()
               : t == AttrType::Vec2 ? reinterpret_cast<float*>(a.write<Vec2>().data())
               : t == AttrType::Vec3 ? reinterpret_cast<float*>(a.write<Vec3>().data())
                                     : reinterpret_cast<float*>(a.write<Vec4>().data());
    for (size_t i = 0; i < n.values.size(); ++i) w[i] = static_cast<float>(n.values[i]);
}

py::dict volumeOf(const std::shared_ptr<const Geometry>& snap, const Volume& v) {
    static const float nothing = 0.0f;
    py::dict d;
    d["name"] = v.name;
    d["origin"] = py::make_tuple(v.origin.x, v.origin.y, v.origin.z);
    d["voxel"] = v.voxel;
    d["resolution"] = py::make_tuple(v.res[0], v.res[1], v.res[2]);
    // Indexed [i, j, k] as the voxels are: x the fastest in memory.
    Array a;
    a.owner = snap;
    a.data = v.values && v.values->size() >= v.count() && v.count() > 0 ? static_cast<const void*>(v.values->data())
                                                                        : static_cast<const void*>(&nothing);
    a.format = "f";
    a.itemsize = static_cast<py::ssize_t>(sizeof(float));
    const bool some = a.data != &nothing;
    a.shape = {some ? v.res[0] : 0, some ? v.res[1] : 0, some ? v.res[2] : 0};
    a.strides = {a.itemsize, a.itemsize * v.res[0], a.itemsize * v.res[0] * v.res[1]};
    d["values"] = py::cast(a);
    return d;
}

}  // namespace

void bindGeometry(py::module_& m) {
    py::class_<PyGeometry>(m, "Geometry", "Points, primitives, their attributes and groups, and volumes.")
        .def(py::init<>())
        .def("copy", [](const PyGeometry& g) { return PyGeometry(g.snapshot()); })
        .def_property_readonly("point_count", [](const PyGeometry& g) { return g.get().pointCount(); })
        .def_property_readonly("vertex_count", [](const PyGeometry& g) { return g.get().vertexCount(); })
        .def_property_readonly("primitive_count", [](const PyGeometry& g) { return g.get().primitiveCount(); })
        .def_property_readonly("volume_count", [](const PyGeometry& g) { return g.get().volumeCount(); })
        // --- attributes
        .def("attribute_names", [](const PyGeometry& g, const std::string& cls) {
            return g.get().attributes(attrClass(cls)).names();
        })
        .def("attribute_type", [](const PyGeometry& g, const std::string& cls, const std::string& name) -> py::object {
            const AttributeArray* a = g.get().attributes(attrClass(cls)).find(name);
            return a ? py::cast(std::string(typeName(a->type()))) : py::none();
        })
        .def("attribute", [](const PyGeometry& g, const std::string& cls, const std::string& name) {
            return attributeOf(g, attrClass(cls), name);
        })
        .def("set_attribute", [](PyGeometry& g, const std::string& cls, const std::string& name, py::handle data,
                                 const std::string& type) { setAttribute(g, attrClass(cls), name, data, type); },
             py::arg("cls"), py::arg("name"), py::arg("data"), py::arg("type") = "")
        .def("remove_attribute", [](PyGeometry& g, const std::string& cls, const std::string& name) {
            if (name == "P") throw Error("P stays: every point has a position");
            return g.edit().attributes(attrClass(cls)).erase(name);
        })
        // --- building
        .def("add_points", [](PyGeometry& g, py::handle points) {
            Geometry& geo = g.edit();
            if (py::isinstance<py::int_>(points)) return geo.addPoints(points.cast<size_t>());
            const std::vector<Vec3> at = vectorsOf(points);
            const size_t first = geo.addPoints(at.size());
            std::copy(at.begin(), at.end(), geo.positionsForWrite().begin() + static_cast<std::ptrdiff_t>(first));
            return first;
        })
        .def("add_primitives", [](PyGeometry& g, py::handle sizes, py::handle points, bool closed) {
            Geometry& geo = g.edit();
            const std::vector<uint32_t> n = indicesOf(sizes, UINT32_MAX, "a primitive's size");
            const std::vector<uint32_t> at = indicesOf(points, geo.pointCount(), "point");
            size_t total = 0;
            for (const uint32_t k : n) total += k;
            if (total != at.size()) {
                throw Error("the sizes add up to " + std::to_string(total) + " corners, the points are " +
                            std::to_string(at.size()));
            }
            const size_t first = geo.primitiveCount();
            size_t k = 0;
            for (const uint32_t count : n) {
                geo.addPrimitive(std::span<const uint32_t>(at.data() + k, count), closed);
                k += count;
            }
            return first;
        }, py::arg("sizes"), py::arg("points"), py::arg("closed") = true)
        .def("append", [](PyGeometry& g, const PyGeometry& other) { g.edit().append(other.get()); })
        // --- topology, without copies
        .def("vertex_points", [](const PyGeometry& g) {
            auto snap = g.snapshot();
            const auto v = snap->vertexPoints();
            return rows(snap, v.data(), v.size(), 1);
        })
        .def("primitive_starts", [](const PyGeometry& g) {
            auto snap = g.snapshot();
            const auto v = snap->primitiveStarts();
            return rows(snap, v.data(), v.size(), 1);
        })
        .def("primitive_sizes", [](const PyGeometry& g) {
            auto snap = g.snapshot();
            const auto v = snap->primitiveSizes();
            return rows(snap, v.data(), v.size(), 1);
        })
        .def("primitive_closed", [](const PyGeometry& g) {
            auto snap = g.snapshot();
            const auto v = snap->primitiveClosedFlags();
            return rows(snap, v.data(), v.size(), 1);
        })
        .def("primitive_points", [](const PyGeometry& g, size_t prim) {
            if (prim >= g.get().primitiveCount()) throw py::index_error("no primitive " + std::to_string(prim));
            const auto p = g.get().primitivePoints(prim);
            return std::vector<uint32_t>(p.begin(), p.end());
        })
        // --- groups
        .def("group_names", [](const PyGeometry& g) { return g.get().groupNames(); })
        .def("group", [](const PyGeometry& g, const std::string& name) -> py::object {
            auto snap = g.snapshot();
            const Group* group = snap->findGroup(name);
            if (!group) return py::none();
            const char* cls = group->classOf() == AttrClass::Point       ? "point"
                              : group->classOf() == AttrClass::Primitive ? "primitive"
                              : group->classOf() == AttrClass::Vertex    ? "vertex"
                                                                         : "detail";
            const auto mask = group->mask();
            return py::make_tuple(cls, rows(snap, mask.data(), mask.size(), 1));
        })
        .def("set_group", [](PyGeometry& g, const std::string& name, const std::string& cls, py::handle members) {
            Geometry& geo = g.edit();
            const AttrClass c = attrClass(cls);
            if (c == AttrClass::Detail) throw Error("a group is of points, vertices or primitives");
            const size_t count = geo.elementCount(c);
            const Numbers n = numbersOf(members, 1);
            if (n.rows != count) {
                throw Error(name + ": " + std::to_string(n.rows) + " flags for " + std::to_string(count) + " elements");
            }
            Group& group = geo.createGroup(name, c);
            for (size_t i = 0; i < count; ++i) group.set(i, n.values[i] != 0.0);
        })
        // --- volumes
        .def("volumes", [](const PyGeometry& g) {
            auto snap = g.snapshot();
            py::list out;
            for (const Volume& v : snap->volumes()) out.append(volumeOf(snap, v));
            return out;
        })
        .def("add_volume", [](PyGeometry& g, const std::string& name, py::handle values, std::array<float, 3> origin,
                              float voxel) {
            if (!(voxel > 0.0f)) throw Error("a voxel has a size above 0");
            const py::buffer_info info = py::reinterpret_borrow<py::buffer>(values).request();
            if (info.ndim != 3) throw Error("a volume's values: an array of three dimensions, [i, j, k]");
            const int nx = static_cast<int>(info.shape[0]), ny = static_cast<int>(info.shape[1]),
                      nz = static_cast<int>(info.shape[2]);
            std::string format = info.format;
            while (!format.empty() && std::string("@=<>!").find(format[0]) != std::string::npos) format.erase(0, 1);
            if (format.size() != 1) throw Error("numbers of the format '" + info.format + "' are not read");
            std::vector<float> data(static_cast<size_t>(nx) * static_cast<size_t>(ny) * static_cast<size_t>(nz));
            const char* base = static_cast<const char*>(info.ptr);
            size_t at = 0;
            for (int k = 0; k < nz; ++k) {
                for (int j = 0; j < ny; ++j) {
                    for (int i = 0; i < nx; ++i) {
                        const char* p = base + i * info.strides[0] + j * info.strides[1] + k * info.strides[2];
                        data[at++] = static_cast<float>(readItem(p, format[0], static_cast<size_t>(info.itemsize)));
                    }
                }
            }
            g.edit().addVolume(Volume::make(name, Vec3(origin[0], origin[1], origin[2]), voxel, nx, ny, nz, std::move(data)));
        }, py::arg("name"), py::arg("values"), py::arg("origin") = std::array<float, 3>{0.0f, 0.0f, 0.0f},
           py::arg("voxel") = 0.1f)
        // --- the whole of it
        .def("hash", [](const PyGeometry& g) { return g.get().hash(); })
        .def("bounds", [](const PyGeometry& g) -> py::object {
            const auto P = g.get().positions();
            if (P.empty()) return py::none();
            Vec3 lo = P[0], hi = P[0];
            for (const Vec3& p : P) {
                lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
            }
            return py::make_tuple(py::make_tuple(lo.x, lo.y, lo.z), py::make_tuple(hi.x, hi.y, hi.z));
        })
        .def("save", [](const PyGeometry& g, const std::string& path) {
            std::string error;
            std::error_code ec;
            const std::filesystem::path parent = std::filesystem::path(path).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent, ec);
            if (!io::writeGeometry(g.get(), path, error)) throw Error(error);
        })
        .def_static("load", [](const std::string& path) {
            PyGeometry g;
            std::string error, ext = std::filesystem::path(path).extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool ok = false;
            if (ext == ".obj") {
                ok = io::readObj(path, g.edit(), error);
            } else if (ext == ".ply") {
                ok = io::readPly(path, g.edit(), error);
            } else {
                error = path + ": geometry is read from .obj and .ply";
            }
            if (!ok) throw Error(error);
            return g;
        });
}

}  // namespace pg::python
