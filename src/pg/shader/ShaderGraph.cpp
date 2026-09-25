#include "pg/shader/ShaderGraph.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace pg::shader {

int ShaderGraph::addNode(const std::string& type, float x, float y, const NodeLibrary* library) {
    GraphNode n;
    n.id = nextId_++;
    n.type = type;
    n.x = x;
    n.y = y;
    if (library) {
        if (const NodeDef* def = library->find(type)) n.version = def->version;
    }
    nodes_.push_back(std::move(n));
    ++revision_;
    return nodes_.back().id;
}

bool ShaderGraph::removeNode(int id) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(), [&](const GraphNode& n) { return n.id == id; });
    if (it == nodes_.end()) return false;
    nodes_.erase(it);
    links_.erase(std::remove_if(links_.begin(), links_.end(),
                                [&](const Link& l) { return l.fromNode == id || l.toNode == id; }),
                 links_.end());
    ++revision_;
    return true;
}

GraphNode* ShaderGraph::node(int id) {
    for (auto& n : nodes_) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const GraphNode* ShaderGraph::node(int id) const {
    return const_cast<ShaderGraph*>(this)->node(id);
}

bool ShaderGraph::wouldCreateCycle(int fromNode, int toNode) const {
    if (fromNode == toNode) return true;
    // Walk upstream from `fromNode`; reaching `toNode` means a loop.
    std::vector<int> stack{fromNode};
    std::set<int> seen{fromNode};
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        for (const Link& l : links_) {
            if (l.toNode != n) continue;
            if (l.fromNode == toNode) return true;
            if (seen.insert(l.fromNode).second) stack.push_back(l.fromNode);
        }
    }
    return false;
}

bool ShaderGraph::connect(int fromNode, const std::string& fromPort, int toNode,
                          const std::string& toPort, const NodeLibrary& library, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    const GraphNode* from = node(fromNode);
    const GraphNode* to = node(toNode);
    if (!from || !to) return fail("no such node");
    const NodeDef* fromDef = library.find(from->type);
    const NodeDef* toDef = library.find(to->type);
    if (!fromDef || !toDef) return fail("unknown node type");
    const OutputDef* out = fromDef->output(fromPort);
    const PortDef* in = toDef->input(toPort);
    if (!out) return fail("'" + fromDef->label + "' has no output '" + fromPort + "'");
    if (!in) return fail("'" + toDef->label + "' has no input '" + toPort + "'");
    if (!convertible(out->type, in->type)) {
        return fail(std::string("cannot connect ") + typeName(out->type) + " to " + typeName(in->type));
    }
    if (wouldCreateCycle(fromNode, toNode)) return fail("that link would close a loop");

    disconnect(toNode, toPort);
    links_.push_back(Link{fromNode, fromPort, toNode, toPort});
    ++revision_;
    return true;
}

bool ShaderGraph::disconnect(int toNode, const std::string& toPort) {
    const size_t before = links_.size();
    links_.erase(std::remove_if(links_.begin(), links_.end(),
                                [&](const Link& l) { return l.toNode == toNode && l.toPort == toPort; }),
                 links_.end());
    if (links_.size() == before) return false;
    ++revision_;
    return true;
}

const Link* ShaderGraph::linkInto(int toNode, const std::string& toPort) const {
    for (const Link& l : links_) {
        if (l.toNode == toNode && l.toPort == toPort) return &l;
    }
    return nullptr;
}

void ShaderGraph::setInput(int id, const std::string& port, const Value& v) {
    GraphNode* n = node(id);
    if (!n) return;
    auto it = n->inputs.find(port);
    if (it != n->inputs.end() && it->second == v) return;
    n->inputs[port] = v;
    ++revision_;
}

void ShaderGraph::setParam(int id, const std::string& param, const std::string& text) {
    GraphNode* n = node(id);
    if (!n) return;
    auto it = n->params.find(param);
    if (it != n->params.end() && it->second == text) return;
    n->params[param] = text;
    ++revision_;
}

// --- file format -------------------------------------------------------------

std::string ShaderGraph::save() const {
    std::ostringstream out;
    out << "pgshadergraph " << kFormatVersion << '\n';
    std::vector<const GraphNode*> sorted;
    for (const auto& n : nodes_) sorted.push_back(&n);
    std::sort(sorted.begin(), sorted.end(),
              [](const GraphNode* a, const GraphNode* b) { return a->id < b->id; });
    for (const GraphNode* n : sorted) {
        out << "node " << n->id << ' ' << n->type << ' ' << n->version << ' '
            << formatFloat(n->x) << ' ' << formatFloat(n->y) << '\n';
        for (const auto& [port, v] : n->inputs) {
            out << "  in " << port;
            for (int i = 0; i < std::max(1, componentCount(v.type)); ++i) {
                out << ' ' << formatFloat(v.v[static_cast<size_t>(i)]);
            }
            out << '\n';
        }
        for (const auto& [param, text] : n->params) out << "  param " << param << ' ' << text << '\n';
    }
    std::vector<Link> links = links_;
    std::sort(links.begin(), links.end(), [](const Link& a, const Link& b) {
        return a.toNode != b.toNode ? a.toNode < b.toNode : a.toPort < b.toPort;
    });
    for (const Link& l : links) {
        out << "link " << l.fromNode << '.' << l.fromPort << " -> " << l.toNode << '.' << l.toPort
            << '\n';
    }
    return out.str();
}

namespace {

bool splitEndpoint(const std::string& s, int& node, std::string& port) {
    const size_t dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= s.size()) return false;
    try {
        size_t used = 0;
        node = std::stoi(s.substr(0, dot), &used);
        if (used != dot) return false;
    } catch (...) {
        return false;
    }
    port = s.substr(dot + 1);
    return true;
}

}  // namespace

bool ShaderGraph::load(const std::string& text, ShaderGraph& out, std::string& error) {
    ShaderGraph g;
    std::istringstream in(text);
    std::string raw;
    int line = 0;
    GraphNode* current = nullptr;
    auto fail = [&](const std::string& msg) {
        error = "line " + std::to_string(line) + ": " + msg;
        return false;
    };

    bool header = false;
    while (std::getline(in, raw)) {
        ++line;
        const size_t hash = raw.find('#');
        std::istringstream ls(hash == std::string::npos ? raw : raw.substr(0, hash));
        std::vector<std::string> w;
        for (std::string t; ls >> t;) w.push_back(t);
        if (w.empty()) continue;

        if (!header) {
            if (w.size() != 2 || w[0] != "pgshadergraph") return fail("not a shader graph file");
            if (w[1] != std::to_string(kFormatVersion)) {
                return fail("format version " + w[1] + " is not supported (this build reads " +
                            std::to_string(kFormatVersion) + ")");
            }
            header = true;
            continue;
        }

        if (w[0] == "node") {
            if (w.size() != 4 && w.size() != 6) return fail("expected 'node <id> <type> <version> [x y]'");
            GraphNode n;
            try {
                n.id = std::stoi(w[1]);
                n.version = std::stoi(w[3]);
            } catch (...) {
                return fail("bad node id or version");
            }
            if (n.id <= 0 || g.node(n.id)) return fail("node id " + w[1] + " is invalid or taken");
            n.type = w[2];
            if (w.size() == 6 && (!parseFloat(w[4], n.x) || !parseFloat(w[5], n.y))) {
                return fail("bad node position");
            }
            g.nextId_ = std::max(g.nextId_, n.id + 1);
            g.nodes_.push_back(std::move(n));
            current = &g.nodes_.back();
        } else if (w[0] == "in") {
            if (!current || w.size() < 3 || w.size() > 6) return fail("expected 'in <port> <numbers>' under a node");
            Value v;
            v.type = vectorType(static_cast<int>(w.size() - 2));
            for (size_t i = 2; i < w.size(); ++i) {
                if (!parseFloat(w[i], v.v[i - 2])) return fail("bad number '" + w[i] + "'");
            }
            current->inputs[w[1]] = v;
        } else if (w[0] == "param") {
            if (!current || w.size() < 3 || w.size() > 6) {
                return fail("expected 'param <name> <value>' under a node");
            }
            std::string text = w[2];  // a name, or up to four numbers
            for (size_t i = 3; i < w.size(); ++i) text += " " + w[i];
            current->params[w[1]] = text;
        } else if (w[0] == "link") {
            Link l;
            if (w.size() != 4 || w[2] != "->" || !splitEndpoint(w[1], l.fromNode, l.fromPort) ||
                !splitEndpoint(w[3], l.toNode, l.toPort)) {
                return fail("expected 'link <node>.<output> -> <node>.<input>'");
            }
            if (!g.node(l.fromNode) || !g.node(l.toNode)) return fail("link to a missing node");
            if (g.linkInto(l.toNode, l.toPort)) return fail("input " + w[3] + " is linked twice");
            g.links_.push_back(std::move(l));
        } else {
            return fail("unknown line '" + w[0] + "'");
        }
    }
    if (!header) return fail("empty file");
    g.revision_ = out.revision_ + 1;
    out = std::move(g);
    return true;
}

}  // namespace pg::shader
