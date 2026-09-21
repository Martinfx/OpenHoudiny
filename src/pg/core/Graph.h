#pragma once
//
// Owns a set of nodes and hands out stable pointers to them.
//
#include "pg/core/Node.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pg {

class Graph {
public:
    Graph();
    ~Graph();

    /// Creates a node of a registered type. Returns nullptr for unknown types
    /// or a duplicate name.
    Node* create(const std::string& typeName, const std::string& name);

    /// Takes ownership of an externally constructed node.
    Node* add(std::unique_ptr<Node> node);

    Node* find(const std::string& name) const;
    std::vector<Node*> nodes() const;
    size_t size() const { return nodes_.size(); }
    void clear();

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    std::map<std::string, Node*> byName_;
};

}  // namespace pg
