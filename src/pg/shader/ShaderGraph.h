#pragma once
//
// A shader graph: node instances and the links between their ports.
//
// The graph only stores what the user built. What a node means lives in the
// NodeLibrary; what the graph compiles to is the Generator's business. Keeping
// the three apart is what lets a user library add nodes, and a new Target add
// a language, without touching this file.
//
// File format (.pgsg) -- plain text, one fact per line, stable order, so graphs
// diff well in git:
//
//   pgshadergraph 1
//   node 1 uv 1 40 80              # id, type, type version, editor x y
//   node 2 checker 1 260 60
//     in scale 12                  # an unconnected input's value
//     in color2 0.9 0.4 0.1
//   node 3 float_parameter 1 40 200
//     param name roughness         # a param's value: a name or numbers
//   node 4 surface_output 1 520 80
//   link 1.uv -> 2.uv
//   link 2.color -> 4.color
//
#include "pg/shader/NodeLibrary.h"

#include <map>
#include <string>
#include <vector>

namespace pg::shader {

struct GraphNode {
    int id = 0;
    std::string type;
    int version = 1;
    float x = 0.0f, y = 0.0f;                    ///< editor position
    std::map<std::string, Value> inputs;         ///< values of unconnected inputs, if changed
    std::map<std::string, std::string> params;   ///< param values, as text
};

struct Link {
    int fromNode = 0;
    std::string fromPort;   ///< an output
    int toNode = 0;
    std::string toPort;     ///< an input; at most one link per input

    bool operator==(const Link& o) const {
        return fromNode == o.fromNode && fromPort == o.fromPort && toNode == o.toNode &&
               toPort == o.toPort;
    }
};

class ShaderGraph {
public:
    /// Adds a node of `type` and returns its id. The type is checked against a
    /// library only when one is given -- a graph can be loaded before the
    /// library that defines its nodes.
    int addNode(const std::string& type, float x = 0.0f, float y = 0.0f,
                const NodeLibrary* library = nullptr);
    /// Removes the node and every link touching it.
    bool removeNode(int id);

    GraphNode* node(int id);
    const GraphNode* node(int id) const;
    const std::vector<GraphNode>& nodes() const { return nodes_; }

    /// Links an output to an input, replacing whatever fed that input before.
    /// Refuses unknown ports, incompatible types and links that would close a
    /// loop -- the generator needs a DAG. `error` says why.
    bool connect(int fromNode, const std::string& fromPort, int toNode, const std::string& toPort,
                 const NodeLibrary& library, std::string* error = nullptr);
    bool disconnect(int toNode, const std::string& toPort);

    const std::vector<Link>& links() const { return links_; }
    /// The link feeding an input, if any.
    const Link* linkInto(int toNode, const std::string& toPort) const;
    /// True if `to` is already upstream of `from`, so from -> to would be a loop.
    bool wouldCreateCycle(int fromNode, int toNode) const;

    /// Sets the value an unconnected input uses, or a param.
    void setInput(int id, const std::string& port, const Value& v);
    void setParam(int id, const std::string& param, const std::string& text);

    std::string save() const;
    /// Replaces `out` with the graph in `text`. Unknown node types are kept --
    /// the generator reports them -- so a graph survives a missing library.
    static bool load(const std::string& text, ShaderGraph& out, std::string& error);

    /// Bumped by every edit, including moving a node. Cheap change detection.
    uint64_t revision() const { return revision_; }

    static constexpr int kFormatVersion = 1;

private:
    std::vector<GraphNode> nodes_;
    std::vector<Link> links_;
    int nextId_ = 1;
    uint64_t revision_ = 0;
};

}  // namespace pg::shader
