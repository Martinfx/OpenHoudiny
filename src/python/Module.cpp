// The extension _pg: arrays, errors, and the parts in the other files.
#include "Bindings.h"

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
}
