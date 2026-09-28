#pragma once
//
// The extension _pg of the Python package pg (src/python/pg/__init__.py wraps
// it in classes of its own): what its parts share.
//
// Arrays go to Python without a copy: an Array points into the memory of a
// geometry or a frame and keeps it alive, and Python reads it through the
// buffer protocol -- numpy.asarray(array) is a numpy array over the same
// bytes, memoryview(array) without numpy. Read only: the memory is shared
// (copy on write, as in the core), and changing it would change every
// geometry that shares it.
//
#include "pg/core/Geometry.h"

#include <pybind11/pybind11.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace pg::python {

namespace py = pybind11;

/// What goes wrong: pg.Error in Python.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Memory Python reads without a copy, kept alive by `owner`.
struct Array {
    std::shared_ptr<const void> owner;
    const void* data = nullptr;
    std::string format;  ///< as struct writes it: "f" float, "e" half, "i" int32, "I" uint32, "B" byte
    py::ssize_t itemsize = 0;
    std::vector<py::ssize_t> shape, strides;  ///< bytes between neighbours along each axis
};

/// `count` rows of `width` items of `T` from `data` on, each `stride` bytes
/// after the one before (0: packed); width 1 makes a flat array.
template <class T>
Array rows(std::shared_ptr<const void> owner, const T* data, size_t count, size_t width, size_t stride = 0) {
    static const T nothing{};  // a place to point at when there is nothing
    Array a;
    a.owner = std::move(owner);
    a.data = count > 0 && data ? static_cast<const void*>(data) : static_cast<const void*>(&nothing);
    a.format = py::format_descriptor<T>::format();
    a.itemsize = static_cast<py::ssize_t>(sizeof(T));
    const py::ssize_t row = static_cast<py::ssize_t>(stride ? stride : sizeof(T) * width);
    a.shape.push_back(static_cast<py::ssize_t>(count));
    a.strides.push_back(row);
    if (width > 1) {
        a.shape.push_back(static_cast<py::ssize_t>(width));
        a.strides.push_back(a.itemsize);
    }
    return a;
}

/// Half floats: the core keeps them as uint16_t, Python reads them as "e".
Array halves(std::shared_ptr<const void> owner, const uint16_t* data, std::vector<py::ssize_t> shape,
             std::vector<py::ssize_t> strides);

/// A geometry Python holds: its own, sharing the buffers of the one it came
/// from (a cook's, a frame's) until it is changed -- then its buffers are
/// its own, as a node's are in the core.
class PyGeometry {
public:
    PyGeometry() : geo_(std::make_shared<Geometry>()) {}
    explicit PyGeometry(const GeometryPtr& from) : geo_(std::make_shared<Geometry>(from ? *from : Geometry())) {}

    const Geometry& get() const { return *geo_; }
    Geometry& edit() { return *geo_; }
    /// As it is now, for a view to keep: changes after it do not reach it.
    std::shared_ptr<const Geometry> snapshot() const { return std::make_shared<const Geometry>(*geo_); }

private:
    std::shared_ptr<Geometry> geo_;
};

/// "point", "vertex", "primitive" (or "prim"), "detail" -- as Houdini names them.
AttrClass attrClass(const std::string& name);

void bindArray(py::module_& m);
void bindGeometry(py::module_& m);
void bindNetwork(py::module_& m);
void bindSimulation(py::module_& m);
void bindUsd(py::module_& m);

}  // namespace pg::python
