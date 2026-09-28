#pragma once
//
// A network Python holds: its nodes, the folder its files are read from, and
// the graph its geometry cooks in -- kept, so that only what changed cooks
// again.
//
#include "Bindings.h"

#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

#include <string>
#include <vector>

namespace pg::python {

struct PyNetwork {
    sim::Network net;
    std::string folder;
    std::vector<std::string> warnings;  ///< what loading it said
    sim::GeometryGraph graph;

    const sim::Node& node(int id) const;
    const sim::ParamDef& param(int id, const std::string& name) const;
    py::object get(int id, const std::string& name) const;
    void set(int id, const std::string& name, py::handle value);
    /// A number, three numbers or text, as a value of the parameter.
    sim::ParamValue valueFrom(int id, const std::string& name, py::handle value) const;
    int outputNode() const;
    /// Seconds a frame, as the Output has it.
    float timeStep() const;
};

}  // namespace pg::python
