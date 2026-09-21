#include "pg/core/Node.h"

#include <algorithm>
#include <unordered_set>

namespace pg {

// --- ParamSet --------------------------------------------------------------

namespace {

template <class T>
bool assignIfChanged(std::map<std::string, ParamValue>& values,
                     const std::string& name, T v) {
    auto it = values.find(name);
    if (it != values.end()) {
        if (const T* cur = std::get_if<T>(&it->second); cur && *cur == v) return false;
        it->second = std::move(v);
        return true;
    }
    values.emplace(name, std::move(v));
    return true;
}

template <class T>
T getOr(const std::map<std::string, ParamValue>& values, const std::string& name,
        const T& fallback) {
    auto it = values.find(name);
    if (it == values.end()) return fallback;
    if (const T* v = std::get_if<T>(&it->second)) return *v;
    return fallback;
}

}  // namespace

bool ParamSet::setFloat(const std::string& n, float v) { return assignIfChanged(values_, n, v); }
bool ParamSet::setInt(const std::string& n, int v) { return assignIfChanged(values_, n, v); }
bool ParamSet::setBool(const std::string& n, bool v) { return assignIfChanged(values_, n, v); }
bool ParamSet::setString(const std::string& n, std::string v) {
    return assignIfChanged(values_, n, std::move(v));
}
bool ParamSet::setVec3(const std::string& n, const Vec3& v) {
    return assignIfChanged(values_, n, v);
}

float ParamSet::getFloat(const std::string& n, float f) const { return getOr(values_, n, f); }
int ParamSet::getInt(const std::string& n, int f) const { return getOr(values_, n, f); }
bool ParamSet::getBool(const std::string& n, bool f) const { return getOr(values_, n, f); }
std::string ParamSet::getString(const std::string& n, const std::string& f) const {
    return getOr(values_, n, f);
}
Vec3 ParamSet::getVec3(const std::string& n, const Vec3& f) const { return getOr(values_, n, f); }

void ParamSet::setExpression(const std::string& name, ParamExpr expr) {
    if (expr) exprs_[name] = std::move(expr);
    else exprs_.erase(name);
}

bool ParamSet::hasExpression(const std::string& name) const {
    return exprs_.count(name) > 0;
}

float ParamSet::evalFloat(const std::string& name, const CookContext& ctx, float fallback) const {
    auto it = exprs_.find(name);
    if (it != exprs_.end()) return static_cast<float>(it->second(ctx));
    return getFloat(name, fallback);
}

Vec3 ParamSet::evalVec3(const std::string& name, const CookContext& ctx,
                        const Vec3& fallback) const {
    Vec3 base = getVec3(name, fallback);
    static const char* kComponents[3] = {".x", ".y", ".z"};
    for (int i = 0; i < 3; ++i) {
        auto it = exprs_.find(name + kComponents[i]);
        if (it != exprs_.end()) base[i] = static_cast<float>(it->second(ctx));
    }
    return base;
}

std::vector<std::string> ParamSet::names() const {
    std::vector<std::string> out;
    out.reserve(values_.size());
    for (const auto& [name, v] : values_) out.push_back(name);
    return out;
}

// --- Node ------------------------------------------------------------------

Node::Node(std::string typeName, std::string name)
    : typeName_(std::move(typeName)), name_(std::move(name)) {}

Node::~Node() {
    // Leave no dangling back-edges if a node outlives its neighbours.
    for (Node* in : inputs_) {
        if (!in) continue;
        auto& o = in->outputs_;
        o.erase(std::remove(o.begin(), o.end(), this), o.end());
    }
    for (Node* out : outputs_) {
        if (!out) continue;
        for (auto& slot : out->inputs_) {
            if (slot == this) slot = nullptr;
        }
    }
}

void Node::setInputCount(size_t n) {
    for (size_t i = n; i < inputs_.size(); ++i) setInput(i, nullptr);
    inputs_.resize(n, nullptr);
}

bool Node::wouldCreateCycle(const Node* source) const {
    if (!source) return false;
    if (source == this) return true;
    std::vector<const Node*> stack{source};
    std::unordered_set<const Node*> seen{source};
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        for (size_t i = 0; i < n->inputs_.size(); ++i) {
            const Node* in = n->inputs_[i];
            if (!in) continue;
            if (in == this) return true;
            if (seen.insert(in).second) stack.push_back(in);
        }
    }
    return false;
}

bool Node::setInput(size_t i, Node* source) {
    if (wouldCreateCycle(source)) return false;
    if (i >= inputs_.size()) inputs_.resize(i + 1, nullptr);
    if (inputs_[i] == source) return true;

    if (Node* old = inputs_[i]) {
        auto& o = old->outputs_;
        o.erase(std::remove(o.begin(), o.end(), this), o.end());
    }
    inputs_[i] = source;
    if (source) source->outputs_.push_back(this);
    bumpVersion();
    return true;
}

void Node::bumpVersion() {
    // Iterative, with a visited set: a diamond-shaped graph would otherwise be
    // walked exponentially.
    std::vector<Node*> stack{this};
    std::unordered_set<Node*> seen{this};
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        n->version_.fetch_add(1, std::memory_order_acq_rel);
        for (Node* out : n->outputs_) {
            if (out && seen.insert(out).second) stack.push_back(out);
        }
    }
}

void Node::setFloat(const std::string& n, float v) {
    editParams([&](ParamSet& p) { return p.setFloat(n, v); });
}
void Node::setInt(const std::string& n, int v) {
    editParams([&](ParamSet& p) { return p.setInt(n, v); });
}
void Node::setBool(const std::string& n, bool v) {
    editParams([&](ParamSet& p) { return p.setBool(n, v); });
}
void Node::setString(const std::string& n, std::string v) {
    editParams([&](ParamSet& p) { return p.setString(n, std::move(v)); });
}
void Node::setVec3(const std::string& n, const Vec3& v) {
    editParams([&](ParamSet& p) { return p.setVec3(n, v); });
}
void Node::setExpression(const std::string& n, ParamExpr expr) {
    params_.setExpression(n, std::move(expr));
    bumpVersion();
}

GeometryPtr Node::cookInstrumented(const CookContext& ctx,
                                   std::span<const GeometryPtr> inputs) {
    cookCount_.fetch_add(1, std::memory_order_relaxed);
    return cookNode(ctx, inputs);
}

// --- NodeRegistry ----------------------------------------------------------

NodeRegistry& NodeRegistry::instance() {
    static NodeRegistry r;
    return r;
}

void NodeRegistry::add(const std::string& typeName, Factory f) {
    factories_[typeName] = std::move(f);
}

std::unique_ptr<Node> NodeRegistry::create(const std::string& typeName,
                                           const std::string& name) const {
    auto it = factories_.find(typeName);
    return it == factories_.end() ? nullptr : it->second(name);
}

std::vector<std::string> NodeRegistry::typeNames() const {
    std::vector<std::string> out;
    out.reserve(factories_.size());
    for (const auto& [name, f] : factories_) out.push_back(name);
    return out;
}

}  // namespace pg
