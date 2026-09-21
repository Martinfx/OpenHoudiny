#include "pg/core/Graph.h"

namespace pg {

Graph::Graph() { registerBuiltinNodes(); }

Graph::~Graph() { clear(); }

Node* Graph::create(const std::string& typeName, const std::string& name) {
    if (byName_.count(name)) return nullptr;
    auto node = NodeRegistry::instance().create(typeName, name);
    if (!node) return nullptr;
    return add(std::move(node));
}

Node* Graph::add(std::unique_ptr<Node> node) {
    if (!node) return nullptr;
    if (byName_.count(node->name())) return nullptr;
    Node* raw = node.get();
    byName_[raw->name()] = raw;
    nodes_.push_back(std::move(node));
    return raw;
}

Node* Graph::find(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : it->second;
}

std::vector<Node*> Graph::nodes() const {
    std::vector<Node*> out;
    out.reserve(nodes_.size());
    for (const auto& n : nodes_) out.push_back(n.get());
    return out;
}

void Graph::clear() {
    // Disconnect first: ~Node fixes up neighbours, and doing that while the
    // vector is already tearing down would touch half-destroyed objects.
    for (auto& n : nodes_) {
        for (size_t i = 0; i < n->inputCount(); ++i) n->setInput(i, nullptr);
    }
    byName_.clear();
    nodes_.clear();
}

}  // namespace pg
