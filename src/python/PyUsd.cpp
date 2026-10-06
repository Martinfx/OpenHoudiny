// USD stages read by the program's own reader (pg/usd): prims, values,
// transforms, geometry and cameras, for a script -- and for checking the
// reader against the library itself where pxr is installed.
#include "Bindings.h"

#include "pg/usd/Geom.h"
#include "pg/usd/Stage.h"

#include <pybind11/stl.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace pg::python {

namespace {

struct PyStage {
    std::shared_ptr<const usd::Stage> stage;

    const usd::Stage::Prim& prim(const std::string& path) const {
        const usd::Stage::Prim* p = stage->find(path);
        if (!p) throw Error("no prim " + path + " on the stage");
        return *p;
    }
};

py::object toPython(const usd::Value& v) {
    using K = usd::Value::Kind;
    switch (v.kind) {
        case K::Numbers: {
            auto element = [&](size_t i) -> py::object {
                if (v.width == 1) return py::float_(v.numbers[i]);
                py::tuple t(static_cast<size_t>(v.width));
                for (int k = 0; k < v.width; ++k) t[static_cast<size_t>(k)] = py::float_(v.numbers[i * static_cast<size_t>(v.width) + static_cast<size_t>(k)]);
                return t;
            };
            if (!v.array) return v.numbers.empty() ? py::object(py::none()) : element(0);
            py::list out;
            for (size_t i = 0; i < v.size(); ++i) out.append(element(i));
            return out;
        }
        case K::Strings: {
            if (!v.array) return py::str(v.text());
            py::list out;
            for (const std::string& s : v.strings) out.append(py::str(s));
            return out;
        }
        case K::Dictionary: {
            py::dict out;
            if (v.dictionary) {
                for (const auto& [k, x] : *v.dictionary) out[py::str(k)] = toPython(x);
            }
            return out;
        }
        default: return py::none();
    }
}

py::list matrix(const usd::Matrix& m) {
    py::list rows;
    for (int r = 0; r < 4; ++r) {
        py::list row;
        for (int c = 0; c < 4; ++c) row.append(m.at(r, c));
        rows.append(row);
    }
    return rows;
}

}  // namespace

void bindUsd(py::module_& m) {
    py::class_<PyStage>(m, "UsdStage", "A USD file composed into its stage by the program's own reader.")
        .def_static(
            "open",
            [](const std::string& path) {
                std::string error;
                std::shared_ptr<const usd::Stage> s;
                {
                    py::gil_scoped_release released;
                    s = usd::Stage::open(path, error);
                }
                if (!s) throw Error(error);
                return PyStage{s};
            },
            py::arg("path"))
        .def("prims",
             [](const PyStage& s) {
                 py::list out;
                 for (const auto& p : s.stage->prims()) {
                     if (p->path == "/") continue;
                     const char* spec = p->specifier == usd::Specifier::Def     ? "def"
                                        : p->specifier == usd::Specifier::Class ? "class"
                                                                                : "over";
                     out.append(py::make_tuple(p->path, p->type, p->defined, p->active, spec));
                 }
                 return out;
             })
        .def("children",
             [](const PyStage& s, const std::string& path) {
                 py::list out;
                 for (const usd::Stage::Prim* c : s.prim(path).children) out.append(c->path);
                 return out;
             })
        .def("property_names", [](const PyStage& s, const std::string& path) { return s.stage->propertyNames(s.prim(path)); })
        .def("type_name",
             [](const PyStage& s, const std::string& path, const std::string& name) -> py::object {
                 const usd::Property* p = s.stage->property(s.prim(path), name);
                 if (!p) return py::none();
                 return py::str(p->relationship ? "rel" : p->typeName);
             })
        .def("value", [](const PyStage& s, const std::string& path, const std::string& name,
                         double time) { return toPython(s.stage->value(s.prim(path), name, time)); },
             py::arg("path"), py::arg("name"), py::arg("time"))
        .def("varies", [](const PyStage& s, const std::string& path, const std::string& name) {
            return s.stage->varies(s.prim(path), name);
        })
        .def("sample_times", [](const PyStage& s, const std::string& path, const std::string& name) {
            return s.stage->sampleTimes(s.prim(path), name);
        })
        .def("metadata",
             [](const PyStage& s, const std::string& path, const std::string& key) -> py::object {
                 const usd::Value* v = s.stage->metadata(s.prim(path), key);
                 return v ? toPython(*v) : py::object(py::none());
             })
        .def("targets", [](const PyStage& s, const std::string& path, const std::string& name) {
            return s.stage->targets(s.prim(path), name);
        })
        .def("local", [](const PyStage& s, const std::string& path, double time) {
            bool resets = false;
            const usd::Matrix m = usd::localTransform(*s.stage, s.prim(path), time, &resets);
            return py::make_tuple(matrix(m), resets);
        })
        .def("world", [](const PyStage& s, const std::string& path, double time) {
            return matrix(usd::worldTransform(*s.stage, s.prim(path), time));
        })
        .def("transform_varies", [](const PyStage& s, const std::string& path) {
            return usd::transformVaries(*s.stage, s.prim(path));
        })
        .def("visible", [](const PyStage& s, const std::string& path, double time) {
            return usd::visible(*s.stage, s.prim(path), time);
        })
        .def("purpose", [](const PyStage& s, const std::string& path) { return usd::purpose(*s.stage, s.prim(path)); })
        .def(
            "geometry",
            [](const PyStage& s, double time, const std::vector<std::string>& roots, bool render, bool proxy, bool guide,
               bool metresYUp, bool subsets, bool pathAttribute, bool materials, int subdivision) {
                usd::ImportOptions o;
                o.roots = roots;
                o.render = render;
                o.proxy = proxy;
                o.guide = guide;
                o.metresYUp = metresYUp;
                o.subsets = subsets;
                o.pathAttribute = pathAttribute;
                o.materials = materials;
                o.subdivision = std::clamp(subdivision, 0, 6);
                std::vector<std::string> notes;
                std::shared_ptr<Geometry> g;
                {
                    py::gil_scoped_release released;
                    g = usd::importGeometry(*s.stage, time, o, &notes);
                }
                return py::make_tuple(PyGeometry(g), notes);
            },
            py::arg("time"), py::arg("roots") = std::vector<std::string>{}, py::arg("render") = true,
            py::arg("proxy") = false, py::arg("guide") = false, py::arg("metres_y_up") = true,
            py::arg("subsets") = true, py::arg("path_attribute") = true, py::arg("materials") = true,
            py::arg("subdivision") = 2)
        .def("geometry_varies",
             [](const PyStage& s, const std::vector<std::string>& roots, bool render, bool proxy, bool guide) {
                 usd::ImportOptions o;
                 o.roots = roots;
                 o.render = render;
                 o.proxy = proxy;
                 o.guide = guide;
                 return usd::geometryVaries(*s.stage, o);
             },
             py::arg("roots") = std::vector<std::string>{}, py::arg("render") = true, py::arg("proxy") = false,
             py::arg("guide") = false)
        .def("cameras",
             [](const PyStage& s) {
                 std::vector<std::string> out;
                 for (const usd::Stage::Prim* p : usd::cameras(*s.stage)) out.push_back(p->path);
                 return out;
             })
        .def(
            "camera",
            [](const PyStage& s, const std::string& path, double time, bool metresYUp) {
                usd::CameraSample c;
                if (!usd::cameraAt(*s.stage, s.prim(path), time, metresYUp, c)) throw Error(path + " is not a camera");
                py::dict d;
                d["world"] = matrix(c.world);
                d["focal_length"] = c.focalLength;
                d["horizontal_aperture"] = c.horizontalAperture;
                d["vertical_aperture"] = c.verticalAperture;
                d["horizontal_aperture_offset"] = c.horizontalApertureOffset;
                d["vertical_aperture_offset"] = c.verticalApertureOffset;
                d["clipping_range"] = py::make_tuple(c.nearClip, c.farClip);
                d["focus_distance"] = c.focusDistance;
                d["f_stop"] = c.fStop;
                d["orthographic"] = c.orthographic;
                d["varies"] = usd::cameraVaries(*s.stage, s.prim(path));
                return d;
            },
            py::arg("path"), py::arg("time"), py::arg("metres_y_up") = true)
        .def_property_readonly("meters_per_unit", [](const PyStage& s) { return s.stage->metersPerUnit(); })
        .def_property_readonly("up_axis", [](const PyStage& s) { return s.stage->zUp() ? "Z" : "Y"; })
        .def_property_readonly("start_time_code", [](const PyStage& s) { return s.stage->startTimeCode(); })
        .def_property_readonly("end_time_code", [](const PyStage& s) { return s.stage->endTimeCode(); })
        .def_property_readonly("has_time_range", [](const PyStage& s) { return s.stage->hasTimeRange(); })
        .def_property_readonly("time_codes_per_second", [](const PyStage& s) { return s.stage->timeCodesPerSecond(); })
        .def_property_readonly("default_prim", [](const PyStage& s) { return s.stage->defaultPrim(); })
        .def_property_readonly("warnings", [](const PyStage& s) { return s.stage->warnings(); })
        .def_property_readonly("files", [](const PyStage& s) { return s.stage->files(); });
}

}  // namespace pg::python
