// pg.Network: the nodes of a network, their parameters, links and flags; the
// node types; geometry cooked at a frame.
#include "Bindings.h"
#include "PyNetwork.h"

#include "pg/sim/Network.h"

#include <pybind11/stl.h>

#include <cmath>

namespace pg::python {

namespace {

const char* kindName(sim::ParamKind k) {
    switch (k) {
        case sim::ParamKind::Float: return "float";
        case sim::ParamKind::Int: return "int";
        case sim::ParamKind::Toggle: return "toggle";
        case sim::ParamKind::Vector: return "vector";
        case sim::ParamKind::Color: return "color";
        case sim::ParamKind::Choice: return "choice";
        case sim::ParamKind::File: return "file";
        case sim::ParamKind::Text: return "text";
        case sim::ParamKind::Code: return "code";
        case sim::ParamKind::Data: return "data";
    }
    return "float";
}

py::object valueOf(const sim::ParamDef& d, const sim::ParamValue& v) {
    switch (d.kind) {
        case sim::ParamKind::Float: return py::float_(v[0]);
        case sim::ParamKind::Int: return py::int_(static_cast<long>(std::lround(v[0])));
        case sim::ParamKind::Toggle: return py::bool_(v[0] != 0.0f);
        case sim::ParamKind::Vector:
        case sim::ParamKind::Color: return py::make_tuple(v[0], v[1], v[2]);
        case sim::ParamKind::Choice: {
            const size_t i = static_cast<size_t>(std::max(0L, std::lround(v[0])));
            return i < d.choices.size() ? py::object(py::str(d.choices[i])) : py::object(py::int_(static_cast<long>(i)));
        }
        default: return py::none();
    }
}

py::list pinsOf(const std::vector<sim::PinDef>& pins) {
    py::list out;
    for (const sim::PinDef& p : pins) out.append(py::make_tuple(p.name, sim::pinTypeName(p.type), p.many));
    return out;
}

py::dict paramInfo(const sim::ParamDef& d) {
    py::dict out;
    out["name"] = d.name;
    out["label"] = d.label;
    out["section"] = d.section;
    out["kind"] = kindName(d.kind);
    out["default"] = sim::isText(d.kind) ? py::object(py::str(d.text)) : valueOf(d, d.value);
    out["min"] = d.min;
    out["max"] = d.max;
    out["unit"] = d.unit;
    out["help"] = d.help;
    std::vector<std::string> choices(d.choices.begin(), d.choices.end());
    out["choices"] = choices;
    return out;
}

py::dict typeInfo(const sim::NodeType& t) {
    py::dict out;
    out["name"] = t.name;
    out["label"] = t.label;
    out["category"] = t.category;
    out["help"] = t.help;
    out["inputs"] = pinsOf(t.inputs);
    out["outputs"] = pinsOf(t.outputs);
    py::list params;
    for (const sim::ParamDef& d : t.params) params.append(paramInfo(d));
    out["params"] = params;
    out["geometry"] = t.core != nullptr;
    return out;
}

sim::Interp interpOf(const std::string& name) {
    if (name == "smooth") return sim::Interp::Smooth;
    if (name == "linear") return sim::Interp::Linear;
    if (name == "step") return sim::Interp::Step;
    throw Error("no interpolation '" + name + "': smooth, linear or step");
}

}  // namespace

const sim::Node& PyNetwork::node(int id) const {
    const sim::Node* n = net.node(id);
    if (!n) throw Error("no node " + std::to_string(id) + " in the network");
    return *n;
}

const sim::ParamDef& PyNetwork::param(int id, const std::string& name) const {
    const sim::ParamDef* d = net.paramDef(id, name);
    if (!d) {
        std::string known;
        for (const sim::ParamDef* p : net.params(id)) known += std::string(known.empty() ? "" : ", ") + p->name;
        throw Error(node(id).name + " has no parameter '" + name + "'; it has " + known);
    }
    return *d;
}

py::object PyNetwork::get(int id, const std::string& name) const {
    const sim::ParamDef& d = param(id, name);
    if (sim::isText(d.kind)) return py::str(net.text(id, name));
    return valueOf(d, net.param(id, name));
}

sim::ParamValue PyNetwork::valueFrom(int id, const std::string& name, py::handle value) const {
    const sim::ParamDef& d = param(id, name);
    sim::ParamValue v{};
    if (py::isinstance<py::str>(value)) {
        std::string error;
        if (!sim::parseParam(d, value.cast<std::string>(), v, error)) throw Error(node(id).name + "." + name + ": " + error);
        return v;
    }
    if (d.kind == sim::ParamKind::Vector || d.kind == sim::ParamKind::Color) {
        if (py::isinstance<py::sequence>(value)) {
            const py::sequence s = py::reinterpret_borrow<py::sequence>(value);
            if (py::len(s) != 3) throw Error(node(id).name + "." + name + ": three numbers");
            for (size_t i = 0; i < 3; ++i) v[i] = py::cast<float>(py::float_(py::reinterpret_borrow<py::object>(s[i])));
        } else {
            v[0] = v[1] = v[2] = py::cast<float>(py::float_(py::reinterpret_borrow<py::object>(value)));  // all three
        }
        return v;
    }
    if (!PyNumber_Check(value.ptr())) {
        throw Error(node(id).name + "." + name + ": a number, not " + py::str(py::type::of(value)).cast<std::string>());
    }
    v[0] = py::cast<float>(py::float_(py::reinterpret_borrow<py::object>(value)));
    return v;
}

void PyNetwork::set(int id, const std::string& name, py::handle value) {
    const sim::ParamDef& d = param(id, name);
    if (sim::isText(d.kind)) {
        if (!py::isinstance<py::str>(value)) throw Error(node(id).name + "." + name + " is text");
        net.setText(id, name, value.cast<std::string>());
        return;
    }
    net.setParam(id, name, valueFrom(id, name, value));
}

int PyNetwork::outputNode() const {
    for (const sim::Node& n : net.nodes()) {
        if (n.type == "output" && !n.bypass) return n.id;
    }
    return 0;
}

float PyNetwork::timeStep() const {
    const int out = outputNode();
    const float fps = out ? net.value(out, "fps") : 30.0f;
    return 1.0f / std::max(fps, 1.0f);
}

void bindNetwork(py::module_& m) {
    m.def("node_types", [] {
        py::list out;
        for (const sim::NodeType* t : sim::allNodeTypes()) out.append(typeInfo(*t));
        return out;
    });
    m.def("examples", [] { return sim::Network::exampleNames(); });
    m.def("examples_folder", [] { return std::string(PG_SIM_EXAMPLES_DIR); });

    py::class_<PyNetwork>(m, "Network", "The nodes of a network: what the editor and the .pgsim files hold.")
        .def(py::init<>())
        .def_static("from_text", [](const std::string& text, const std::string& folder) {
            auto n = std::make_unique<PyNetwork>();
            std::string error;
            std::vector<std::string> warnings;
            if (!sim::Network::load(text, n->net, error, &warnings)) throw Error(error);
            n->folder = folder;
            n->warnings = warnings;
            return n;
        }, py::arg("text"), py::arg("folder") = "")
        .def_static("example", [](const std::string& name) {
            auto n = std::make_unique<PyNetwork>();
            if (!sim::Network::example(name, n->net)) {
                std::string known;
                for (const std::string& e : sim::Network::exampleNames()) known += " " + e;
                throw Error("no example '" + name + "'; there are" + known);
            }
            n->folder = PG_SIM_EXAMPLES_DIR;
            return n;
        })
        .def("text", [](const PyNetwork& n) { return n.net.save(); })
        .def_readwrite("folder", &PyNetwork::folder)
        .def_readonly("warnings", &PyNetwork::warnings)
        .def_property_readonly("revision", [](const PyNetwork& n) { return n.net.revision(); })
        // --- nodes
        .def("add", [](PyNetwork& n, const std::string& type, const std::string& name) {
            if (!sim::findNodeType(type)) throw Error("no node type '" + type + "' (pg.node_types() has them)");
            const int id = n.net.add(type);
            if (!name.empty()) {
                std::string error;
                if (!n.net.rename(id, name, &error)) {
                    n.net.remove(id);
                    throw Error(error);
                }
            }
            return id;
        }, py::arg("type"), py::arg("name") = "")
        .def("remove", [](PyNetwork& n, int id) {
            n.node(id);
            n.net.remove(id);
        })
        .def("ids", [](const PyNetwork& n) {
            std::vector<int> out;
            for (const sim::Node& x : n.net.nodes()) out.push_back(x.id);
            return out;
        })
        .def("find", [](const PyNetwork& n, const std::string& name) {
            const sim::Node* x = n.net.named(name);
            return x ? x->id : 0;
        })
        .def("name", [](const PyNetwork& n, int id) { return n.node(id).name; })
        .def("type", [](const PyNetwork& n, int id) { return n.node(id).type; })
        .def("rename", [](PyNetwork& n, int id, const std::string& name) {
            n.node(id);
            std::string error;
            if (!n.net.rename(id, name, &error)) throw Error(error);
        })
        .def("position", [](const PyNetwork& n, int id) { return py::make_tuple(n.node(id).x, n.node(id).y); })
        .def("set_position", [](PyNetwork& n, int id, float x, float y) {
            n.node(id);
            n.net.node(id)->x = x;
            n.net.node(id)->y = y;
        })
        // --- parameters
        .def("param_names", [](const PyNetwork& n, int id) {
            n.node(id);
            std::vector<std::string> out;
            for (const sim::ParamDef* d : n.net.params(id)) out.push_back(d->name);
            return out;
        })
        .def("param_info", [](const PyNetwork& n, int id, const std::string& name) { return paramInfo(n.param(id, name)); })
        .def("get", &PyNetwork::get)
        .def("get_at", [](const PyNetwork& n, int id, const std::string& name, float frame) {
            const sim::ParamDef& d = n.param(id, name);
            if (sim::isText(d.kind)) return py::object(py::str(n.net.text(id, name)));
            return valueOf(d, n.net.valueAt(id, name, frame));
        })
        .def("set", &PyNetwork::set)
        .def("reset", [](PyNetwork& n, int id, const std::string& name) {
            n.param(id, name);
            n.net.resetParam(id, name);
        })
        .def("is_default", [](const PyNetwork& n, int id, const std::string& name) {
            n.param(id, name);
            return n.net.isDefault(id, name);
        })
        // --- expressions and keys
        .def("expression", [](const PyNetwork& n, int id, const std::string& channel) {
            n.node(id);
            return n.net.expression(id, channel);
        })
        .def("set_expression", [](PyNetwork& n, int id, const std::string& channel, const std::string& text) {
            n.node(id);
            if (!n.net.setExpression(id, channel, text)) {
                std::string known;
                for (const sim::ParamDef* d : n.net.params(id)) {
                    for (const std::string& c : sim::Network::channels(*d)) known += " " + c;
                }
                throw Error(n.node(id).name + " has no channel '" + channel + "'; it has" + known);
            }
            const std::string error = n.net.expressionError(id, channel);
            if (!error.empty()) throw Error(n.node(id).name + "." + channel + ": " + error);
        })
        .def("channels", [](const PyNetwork& n, int id, const std::string& name) {
            return sim::Network::channels(n.param(id, name));
        })
        .def("keys", [](const PyNetwork& n, int id, const std::string& name) {
            const sim::ParamDef& d = n.param(id, name);
            py::list out;
            if (const auto* keys = n.net.keys(id, name)) {
                for (const sim::Key& k : *keys) out.append(py::make_tuple(k.frame, valueOf(d, k.value), sim::interpName(k.interp)));
            }
            return out;
        })
        .def("set_key", [](PyNetwork& n, int id, const std::string& name, float frame, py::handle value,
                           const std::string& interp) {
            const sim::ParamDef& d = n.param(id, name);
            if (sim::isText(d.kind)) throw Error(n.node(id).name + "." + name + " is text: it has no keys");
            n.net.setKey(id, name, frame, n.valueFrom(id, name, value), interpOf(interp));
        }, py::arg("id"), py::arg("name"), py::arg("frame"), py::arg("value"), py::arg("interp") = "smooth")
        .def("clear_keys", [](PyNetwork& n, int id, const std::string& name) {
            n.param(id, name);
            n.net.clearKeys(id, name);
        })
        // --- links
        .def("inputs", [](const PyNetwork& n, int id) {
            const sim::NodeType* t = sim::findNodeType(n.node(id).type);
            return t ? pinsOf(t->inputs) : py::list();
        })
        .def("outputs", [](const PyNetwork& n, int id) {
            const sim::NodeType* t = sim::findNodeType(n.node(id).type);
            return t ? pinsOf(t->outputs) : py::list();
        })
        .def("connect", [](PyNetwork& n, int from, const std::string& output, int to, const std::string& input) {
            n.node(from);
            n.node(to);
            std::string error;
            if (!n.net.connect(from, output, to, input, &error)) throw Error(error);
        })
        .def("disconnect", [](PyNetwork& n, int from, const std::string& output, int to, const std::string& input) {
            return n.net.disconnect(sim::Link{from, output, to, input});
        })
        .def("links", [](const PyNetwork& n) {
            py::list out;
            for (const sim::Link& l : n.net.links()) out.append(py::make_tuple(l.from, l.output, l.to, l.input));
            return out;
        })
        // --- flags
        .def("set_display", [](PyNetwork& n, int id) {
            if (id) n.node(id);
            if (!n.net.setDisplay(id)) throw Error(n.node(id).name + " is not a geometry node: it has no geometry to show");
        })
        .def("displayed", [](const PyNetwork& n) { return n.net.displayed(); })
        .def("bypass", [](const PyNetwork& n, int id) { return n.node(id).bypass; })
        .def("set_bypass", [](PyNetwork& n, int id, bool on) {
            n.node(id);
            if (!n.net.setBypass(id, on)) throw Error(n.node(id).name + " cannot be bypassed");
        })
        // --- what it makes
        .def("problems", [](const PyNetwork& n) {
            const sim::Compiled c = n.net.compile(n.folder);
            py::list out;
            for (const sim::Problem& p : c.problems) {
                out.append(py::make_tuple(p.level == sim::Problem::Level::Error ? "error" : "warning", p.node, p.message));
            }
            return py::make_tuple(c.ok, out);
        })
        .def_property_readonly("time_step", &PyNetwork::timeStep)
        .def("cook", [](PyNetwork& n, int id, int frame) {
            n.node(id);
            GeometryPtr g;
            std::string error;
            {
                py::gil_scoped_release release;
                n.graph.sync(n.net, n.folder);
                if (n.graph.contains(id)) {
                    g = n.graph.cook(id, frame, n.timeStep());
                    error = n.graph.error(id);
                }
            }
            if (!n.graph.contains(id)) throw Error(n.node(id).name + " is not a geometry node");
            if (!error.empty()) throw Error(n.node(id).name + ": " + error);
            return PyGeometry(g);
        }, py::arg("id"), py::arg("frame") = 1)
        .def("errors", [](const PyNetwork& n) {
            py::list out;
            for (const sim::Node& x : n.net.nodes()) {
                const std::string e = n.graph.error(x.id);
                if (!e.empty()) out.append(py::make_tuple(x.id, e));
            }
            return out;
        });
}

}  // namespace pg::python
