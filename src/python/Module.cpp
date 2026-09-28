// The extension _pg: arrays, errors, and the parts in the other files.
#include "Bindings.h"

#include "pg/io/Picture.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Asset.h"

#include <pybind11/stl.h>

#include <string>
#include <vector>

namespace pg::python {

Array halves(std::shared_ptr<const void> owner, const uint16_t* data, std::vector<py::ssize_t> shape,
             std::vector<py::ssize_t> strides) {
    static const uint16_t nothing = 0;
    Array a;
    a.owner = std::move(owner);
    py::ssize_t count = 1;
    for (const py::ssize_t n : shape) count *= n;
    a.data = count > 0 && data ? static_cast<const void*>(data) : static_cast<const void*>(&nothing);
    a.format = "e";
    a.itemsize = 2;
    a.shape = std::move(shape);
    a.strides = std::move(strides);
    return a;
}

AttrClass attrClass(const std::string& name) {
    if (name == "point" || name == "points") return AttrClass::Point;
    if (name == "vertex" || name == "vertices") return AttrClass::Vertex;
    if (name == "primitive" || name == "primitives" || name == "prim" || name == "prims") return AttrClass::Primitive;
    if (name == "detail" || name == "global") return AttrClass::Detail;
    throw Error("no class of attributes '" + name + "': point, vertex, primitive or detail");
}

void bindArray(py::module_& m) {
    py::class_<Array>(m, "Array", py::buffer_protocol(),
                      "Memory of a geometry or a frame, read without a copy: numpy.asarray() of it, or a memoryview.")
        .def_buffer([](const Array& a) {
            return py::buffer_info(const_cast<void*>(a.data), a.itemsize, a.format,
                                   static_cast<py::ssize_t>(a.shape.size()), a.shape, a.strides, true);
        })
        .def_property_readonly("shape", [](const Array& a) { return py::tuple(py::cast(a.shape)); })
        .def_property_readonly("format", [](const Array& a) { return a.format; })
        .def("__len__", [](const Array& a) { return a.shape.empty() ? 0 : a.shape[0]; });
}

}  // namespace pg::python

PYBIND11_MODULE(_pg, m) {
    namespace py = pybind11;
    using namespace pg::python;
    m.doc() = "Prototype's core: networks, geometry, simulations (use the package pg, which wraps it).";
    pg::registerBuiltinNodes();
    py::register_exception<Error>(m, "Error", PyExc_RuntimeError);
    // The digital assets, as the program has them: those it carries, and
    // those of $PROTOTYPE_ASSETS and the user's folder.
    std::vector<std::string> assetErrors;
    pg::sim::AssetLibrary::instance().loadDefaults(&assetErrors);
    m.attr("asset_errors") = assetErrors;
    m.def("assets", [] {
        py::list out;
        for (const auto& a : pg::sim::AssetLibrary::instance().all()) {
            out.append(py::make_tuple(a->name, a->version, a->file));
        }
        return out;
    });
    m.def("load_assets", [](const std::string& folder) {
        std::vector<std::string> errors;
        const int read = pg::sim::AssetLibrary::instance().loadFolder(folder, errors);
        return py::make_tuple(read, errors);
    });
    bindArray(m);
    bindGeometry(m);
    bindNetwork(m);
    bindSimulation(m);
    bindUsd(m);
    m.def(
        "read_picture",
        [](const std::string& path) {
            auto picture = std::make_shared<pg::io::Picture>();
            std::string error;
            {
                py::gil_scoped_release released;
                if (!pg::io::readPicture(path, *picture, error)) picture.reset();
            }
            if (!picture) throw Error(error);
            Array a;
            a.owner = picture;
            a.data = picture->rgba.data();
            a.format = py::format_descriptor<float>::format();
            a.itemsize = sizeof(float);
            const py::ssize_t w = picture->width, h = picture->height;
            a.shape = {h, w, 4};
            a.strides = {w * 16, 16, 4};
            return py::make_tuple(a, picture->linear);
        },
        py::arg("path"), "A PNG, JPEG or OpenEXR file: its pixels (rows x columns x RGBA floats) and whether they are linear.");
    m.def(
        "write_picture",
        [](const std::string& path, py::buffer pixels, int quality) {
            const py::buffer_info info = pixels.request();
            if (info.ndim != 2 && info.ndim != 3) {
                throw Error("a picture is rows x columns, or rows x columns x channels (1 to 4)");
            }
            const py::ssize_t h = info.shape[0], w = info.shape[1], channels = info.ndim == 3 ? info.shape[2] : 1;
            if (channels < 1 || channels > 4) throw Error("a picture has 1 to 4 channels, not " + std::to_string(channels));
            std::string format = info.format;
            while (!format.empty() && std::string("@=<>!").find(format[0]) != std::string::npos) format.erase(0, 1);
            const char kind = format.size() == 1 ? format[0] : '?';
            const bool known = (kind == 'f' && info.itemsize == 4) || (kind == 'd' && info.itemsize == 8) ||
                               (kind == 'B' && info.itemsize == 1) || (kind == 'H' && info.itemsize == 2);
            if (!known) {
                throw Error("a picture's pixels are float32, float64, uint8 or uint16 (numpy: .astype('float32')), not '" +
                            info.format + "'");
            }
            // Bytes are 0 to 255 of what the picture shows, as floats are 0 to 1.
            const double scale = kind == 'B' ? 1.0 / 255.0 : kind == 'H' ? 1.0 / 65535.0 : 1.0;
            const auto item = [&](const char* at) -> float {
                switch (kind) {
                    case 'f': return *reinterpret_cast<const float*>(at);
                    case 'd': return static_cast<float>(*reinterpret_cast<const double*>(at));
                    case 'B': return static_cast<float>(*reinterpret_cast<const uint8_t*>(at) * scale);
                    default: return static_cast<float>(*reinterpret_cast<const uint16_t*>(at) * scale);
                }
            };
            pg::io::Picture picture;
            picture.width = static_cast<int>(w);
            picture.height = static_cast<int>(h);
            picture.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 1.0f);
            const char* base = static_cast<const char*>(info.ptr);
            for (py::ssize_t y = 0; y < h; ++y) {
                for (py::ssize_t x = 0; x < w; ++x) {
                    float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    for (py::ssize_t c = 0; c < channels; ++c) {
                        v[c] = item(base + y * info.strides[0] + x * info.strides[1] + (info.ndim == 3 ? c * info.strides[2] : 0));
                    }
                    float* out = &picture.rgba[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4];
                    if (channels <= 2) {
                        out[0] = out[1] = out[2] = v[0];  // grey, and its alpha
                        out[3] = channels == 2 ? v[1] : 1.0f;
                    } else {
                        for (int c = 0; c < 4; ++c) out[c] = v[c];
                    }
                }
            }
            std::string error;
            bool written = false;
            {
                py::gil_scoped_release released;
                written = pg::io::writePicture(path, picture, quality, error);
            }
            if (!written) throw Error(error);
        },
        py::arg("path"), py::arg("pixels"), py::arg("quality") = 92,
        "Pixels (rows x columns x 1 to 4 channels) to a PNG, JPEG or OpenEXR file, the kind its name says.");
}
