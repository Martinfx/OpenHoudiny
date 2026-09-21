#pragma once
//
// Node and parameter model.
//
// Dirty tracking uses a version counter rather than a boolean flag. Changing a
// parameter bumps this node's version and every downstream node's version; the
// cook cache keys on (node, version, frame), so stale entries simply stop being
// found. No explicit cache invalidation walk, and no way to forget one.
//
#include "pg/core/Geometry.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace pg {

/// What is being asked for: which moment in time, and (later) which element of
/// an enclosing loop. Passed down the whole pull evaluation.
struct CookContext {
    double time = 0.0;  ///< seconds
    int frame = 1;
    double fps = 24.0;
};

using ParamValue = std::variant<int, float, bool, std::string, Vec3>;

/// A parameter may hold a constant, or be driven by an expression. An
/// expression makes the owning node time dependent, which propagates
/// downstream and switches its cache entries to per-frame.
using ParamExpr = std::function<double(const CookContext&)>;

class ParamSet {
public:
    /// Each setter returns true if the stored value actually changed.
    /// Setting a parameter to the value it already has must not dirty anything.
    bool setFloat(const std::string& name, float v);
    bool setInt(const std::string& name, int v);
    bool setBool(const std::string& name, bool v);
    bool setString(const std::string& name, std::string v);
    bool setVec3(const std::string& name, const Vec3& v);

    float getFloat(const std::string& name, float fallback = 0.0f) const;
    int getInt(const std::string& name, int fallback = 0) const;
    bool getBool(const std::string& name, bool fallback = false) const;
    std::string getString(const std::string& name, const std::string& fallback = {}) const;
    Vec3 getVec3(const std::string& name, const Vec3& fallback = {}) const;

    /// Binds an expression. Pass {} to unbind.
    void setExpression(const std::string& name, ParamExpr expr);
    bool hasExpression(const std::string& name) const;
    bool anyExpression() const { return !exprs_.empty(); }

    /// Float value at `ctx`, evaluating the expression if one is bound.
    float evalFloat(const std::string& name, const CookContext& ctx,
                    float fallback = 0.0f) const;
    /// Per-component expressions are named "<name>.x" / ".y" / ".z".
    Vec3 evalVec3(const std::string& name, const CookContext& ctx,
                  const Vec3& fallback = {}) const;

    bool contains(const std::string& name) const { return values_.count(name) > 0; }
    std::vector<std::string> names() const;

private:
    std::map<std::string, ParamValue> values_;
    std::map<std::string, ParamExpr> exprs_;
};

class Node {
public:
    Node(std::string typeName, std::string name);
    virtual ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    const std::string& typeName() const { return typeName_; }
    const std::string& name() const { return name_; }

    // --- wiring -------------------------------------------------------------

    void setInputCount(size_t n);
    size_t inputCount() const { return inputs_.size(); }
    Node* input(size_t i) const { return i < inputs_.size() ? inputs_[i] : nullptr; }
    /// Rewiring dirties this node and everything downstream.
    /// Refuses (and returns false) if the connection would create a cycle --
    /// the evaluator assumes a DAG and must never be handed one that isn't.
    bool setInput(size_t i, Node* source);
    /// True if wiring `source` into this node would close a loop.
    bool wouldCreateCycle(const Node* source) const;
    std::span<Node* const> outputs() const { return outputs_; }

    // --- parameters ---------------------------------------------------------

    /// Mutating access. Any change that actually alters a value bumps the
    /// version of this node and of every node downstream.
    template <class Fn>
    void editParams(Fn&& fn) {
        if (fn(params_)) bumpVersion();
    }
    const ParamSet& params() const { return params_; }

    /// Convenience wrappers that dirty correctly.
    void setFloat(const std::string& name, float v);
    void setInt(const std::string& name, int v);
    void setBool(const std::string& name, bool v);
    void setString(const std::string& name, std::string v);
    void setVec3(const std::string& name, const Vec3& v);
    void setExpression(const std::string& name, ParamExpr expr);

    // --- dirty state --------------------------------------------------------

    uint64_t version() const { return version_.load(std::memory_order_acquire); }
    /// Bumps this node's version and propagates downstream.
    void bumpVersion();

    // --- instrumentation ----------------------------------------------------

    /// Number of times cookNode() has actually run. The dirty-propagation
    /// tests assert on this.
    uint64_t cookCount() const { return cookCount_.load(std::memory_order_relaxed); }
    void resetCookCount() { cookCount_.store(0, std::memory_order_relaxed); }

    // --- the work -----------------------------------------------------------

    /// True if this node's own output varies with time, ignoring its inputs.
    virtual bool isTimeDependentSelf() const { return params_.anyExpression(); }

    /// Produces this node's output. Inputs are already cooked. Must be pure:
    /// same context + same inputs -> same output, no observable side effects.
    virtual GeometryPtr cookNode(const CookContext& ctx,
                                 std::span<const GeometryPtr> inputs) = 0;

    /// Called by the engine; counts the cook and delegates to cookNode().
    GeometryPtr cookInstrumented(const CookContext& ctx,
                                 std::span<const GeometryPtr> inputs);

protected:
    ParamSet params_;

private:
    std::string typeName_;
    std::string name_;
    std::vector<Node*> inputs_;
    std::vector<Node*> outputs_;
    std::atomic<uint64_t> version_{1};
    std::atomic<uint64_t> cookCount_{0};
};

/// Factory registry so graphs can be built from type names (files, CLI, tests).
class NodeRegistry {
public:
    using Factory = std::function<std::unique_ptr<Node>(const std::string& name)>;

    static NodeRegistry& instance();
    void add(const std::string& typeName, Factory f);
    std::unique_ptr<Node> create(const std::string& typeName, const std::string& name) const;
    std::vector<std::string> typeNames() const;

private:
    std::map<std::string, Factory> factories_;
};

/// Registers every built-in node type. Idempotent.
void registerBuiltinNodes();

}  // namespace pg
