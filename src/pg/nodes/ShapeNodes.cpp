//
// The shape grammar as nodes: one node per production rule, after the node
// editor in "Generating 3d buildings using node graphs" (nelari.us, 2020).
//
// Shapes travel through the graph as a single stream. A rule node rewrites the
// shapes whose symbol matches its `shape` parameter and passes every other
// shape through untouched, so a chain of rule nodes is a grammar with one rule
// per node. The `shapegrammar` node runs the same rules written as text. Both
// call the same operations, which is why the same grammar written either way
// produces bit-identical geometry -- tests/test_shape_grammar.cpp checks it.
//
#include "pg/nodes/Nodes.h"

#include "pg/grammar/Grammar.h"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace pg {
namespace {

using namespace grammar;

GeometryPtr firstInput(std::span<const GeometryPtr> in) {
    return in.empty() ? nullptr : in[0];
}

/// Rewrites the shapes whose symbol is `symbol` (every shape, if it is empty)
/// and passes the rest through. A rule that matches nothing returns its input
/// as it is -- not even the attribute arrays are touched (I1).
template <class Emit>
GeometryPtr applyRule(const GeometryPtr& in, const std::string& symbol, Emit&& emit) {
    if (!in) return std::make_shared<Geometry>();
    const ShapeView view(*in);
    const std::vector<uint8_t> selected = view.select(symbol);
    if (std::find(selected.begin(), selected.end(), 1) == selected.end()) return in;

    auto successors = rewrite(view, [&](size_t i, std::vector<Successor>& out) {
        if (!selected[i]) return false;
        emit(view, static_cast<uint32_t>(i), out);
        return true;
    });
    return materialize(*in, successors);
}

/// A node whose input can be wrong -- a split pattern, a rule text. The last
/// problem stays readable through grammarError(); the geometry passes through.
class GrammarNode : public Node {
public:
    using Node::Node;

    std::string lastError() const {
        std::lock_guard<std::mutex> lk(errorMu_);
        return error_;
    }

protected:
    void setError(std::string error) const {
        std::lock_guard<std::mutex> lk(errorMu_);
        error_ = std::move(error);
    }

private:
    mutable std::mutex errorMu_;
    mutable std::string error_;
};

// --- axioms: where shapes come from ------------------------------------------

/// A single box-shaped shape -- the starting point of a grammar. With a zero
/// size.y it is a footprint, ready to be extruded.
class AxiomNode : public Node {
public:
    explicit AxiomNode(std::string name) : Node("axiom", std::move(name)) {
        setInputCount(0);
        params_.setVec3("origin", Vec3(0, 0, 0));
        params_.setVec3("size", Vec3(1, 1, 1));
        params_.setString("name", "Axiom");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr>) override {
        const std::string symbol = params_.getString("name", "Axiom");
        Shape s;
        s.scope.origin = params_.evalVec3("origin", ctx, Vec3(0, 0, 0));
        const Vec3 size = params_.evalVec3("size", ctx, Vec3(1, 1, 1));
        s.scope.size = Vec3(std::max(size.x, 0.0f), std::max(size.y, 0.0f), std::max(size.z, 0.0f));

        Geometry seed;
        seed.addPoints(1);
        return materialize(seed, {Successor{0, s, &symbol, true}});
    }
};

/// One shape per input polygon, fitted to it like a building lot: flat, facing
/// up, x along the polygon's first edge. Primitive attributes of the polygon
/// become attributes of its shape -- and so of everything built on it.
class LotNode : public Node {
public:
    explicit LotNode(std::string name) : Node("lot", std::move(name)) {
        setInputCount(1);
        params_.setString("name", "Lot");
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        const GeometryPtr polys = firstInput(in);
        if (!polys) return std::make_shared<Geometry>();
        const std::string symbol = params_.getString("name", "Lot");

        std::vector<uint32_t> source;
        std::vector<Successor> successors;
        const auto P = polys->positions();
        std::vector<Vec3> ring;
        for (size_t prim = 0; prim < polys->primitiveCount(); ++prim) {
            ring.clear();
            for (uint32_t pt : polys->primitivePoints(prim)) ring.push_back(P[pt]);
            Shape s;
            if (!fitPolygon(ring, s.scope)) continue;
            successors.push_back(Successor{static_cast<uint32_t>(source.size()), s, &symbol, true});
            source.push_back(static_cast<uint32_t>(prim));
        }

        // The seed has one point per lot, carrying its polygon's attributes.
        Geometry seed;
        seed.detail() = polys->detail();
        AttributeSet attrs = polys->primitives();
        attrs.gather(source);
        seed.points() = std::move(attrs);
        seed.points().create("P", AttrType::Vec3);
        return materialize(seed, successors);
    }
};

// --- refinement: one shape in, one shape out --------------------------------

/// Grows flat shapes into volumes; a negative amount digs a recess instead.
class ExtrudeNode : public Node {
public:
    explicit ExtrudeNode(std::string name) : Node("extrude", std::move(name)) {
        setInputCount(1);
        params_.setString("shape", "Lot");
        params_.setFloat("amount", 10.0f);
        params_.setString("attrib", "");  // per-shape amount, overriding `amount`
        params_.setString("name", "Mass");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        const GeometryPtr shapes = firstInput(in);
        const float amount = params_.evalFloat("amount", ctx, 10.0f);
        const std::string attrib = params_.getString("attrib", "");
        const AttributeArray* perShape =
            shapes && !attrib.empty() ? shapes->points().find(attrib) : nullptr;
        const std::string name = params_.getString("name", "");
        const std::string* successor = name.empty() ? nullptr : &name;

        return applyRule(shapes, params_.getString("shape", ""),
                         [&](const ShapeView& v, uint32_t i, std::vector<Successor>& out) {
                             const float d = attributeValue(perShape, i, amount);
                             out.push_back(Successor{i, extrude(v.shape(i), d), successor, true});
                         });
    }
};

/// Puts a hip or gable roof over flat shapes.
class RoofNode : public Node {
public:
    explicit RoofNode(std::string name) : Node("roof", std::move(name)) {
        setInputCount(1);
        params_.setString("shape", "RoofBase");
        params_.setString("type", "hip");
        params_.setFloat("angle", 30.0f);
        params_.setString("attrib", "");  // per-shape angle, overriding `angle`
        params_.setString("name", "Roof");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        const GeometryPtr shapes = firstInput(in);
        const RoofType type =
            params_.getString("type", "hip") == "gable" ? RoofType::Gable : RoofType::Hip;
        const float angle = params_.evalFloat("angle", ctx, 30.0f);
        const std::string attrib = params_.getString("attrib", "");
        const AttributeArray* perShape =
            shapes && !attrib.empty() ? shapes->points().find(attrib) : nullptr;
        const std::string name = params_.getString("name", "");
        const std::string* successor = name.empty() ? nullptr : &name;

        return applyRule(shapes, params_.getString("shape", ""),
                         [&](const ShapeView& v, uint32_t i, std::vector<Successor>& out) {
                             const float a = attributeValue(perShape, i, angle);
                             out.push_back(Successor{i, roof(v.shape(i), type, a), successor, true});
                         });
    }
};

// --- division: one shape in, many out ---------------------------------------

/// Splits shapes along an axis by a pattern such as
/// "{ 4: Ground | { ~3.2: Floor }* | 0.5: Cornice }". The article's "split"
/// and "repeat split" are both special cases of it.
class SplitNode : public GrammarNode {
public:
    explicit SplitNode(std::string name) : GrammarNode("split", std::move(name)) {
        setInputCount(1);
        params_.setString("shape", "Facade");
        params_.setString("axis", "y");
        params_.setString("pattern", "{ ~3: Floor }*");
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        const GeometryPtr shapes = firstInput(in);
        const std::string axisName = params_.getString("axis", "y");
        const char a = axisName.empty() ? 'y' : static_cast<char>(std::tolower(
                                                     static_cast<unsigned char>(axisName[0])));
        const int axis = a == 'x' ? 0 : (a == 'z' ? 2 : 1);

        SplitPattern pattern;
        std::string error;
        if (!parseSplitPattern(params_.getString("pattern", ""), pattern, error)) {
            setError(error);
            return shapes ? shapes : std::make_shared<Geometry>();
        }
        setError({});
        return applyRule(shapes, params_.getString("shape", ""),
                         [&](const ShapeView& v, uint32_t i, std::vector<Successor>& out) {
                             emitSplit(i, v.shape(i), axis, pattern, out);
                         });
    }
};

/// The component split: volumes into their faces, each face named by where it
/// points. The article's "face selection" filter does the same job by normal.
class CompNode : public Node {
public:
    explicit CompNode(std::string name) : Node("comp", std::move(name)) {
        setInputCount(1);
        params_.setString("shape", "Mass");
        for (int f = 0; f < kFaceCount; ++f) params_.setString(faceName(static_cast<Face>(f)), "");
        params_.setString("side", "Facade");
        params_.setString("top", "RoofBase");
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        CompTargets targets;
        for (int f = 0; f < kFaceCount; ++f) {
            targets.face[f] = params_.getString(faceName(static_cast<Face>(f)), "");
        }
        targets.side = params_.getString("side", "");

        return applyRule(firstInput(in), params_.getString("shape", ""),
                         [&](const ShapeView& v, uint32_t i, std::vector<Successor>& out) {
                             emitComp(i, v.shape(i), targets, out);
                         });
    }
};

// --- leaving the shape world ------------------------------------------------

/// Turns shapes into polygons, each carrying its shape's attributes.
class ShapeMeshNode : public Node {
public:
    explicit ShapeMeshNode(std::string name) : Node("shapemesh", std::move(name)) {
        setInputCount(1);
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        const GeometryPtr shapes = firstInput(in);
        return shapes ? mesh(*shapes) : std::make_shared<Geometry>();
    }
};

// --- the text front-end -----------------------------------------------------

/// Runs a whole grammar written as text (see Grammar.h for the syntax).
class ShapeGrammarNode : public GrammarNode {
public:
    explicit ShapeGrammarNode(std::string name) : GrammarNode("shapegrammar", std::move(name)) {
        setInputCount(1);
        params_.setString("rules", "");
        params_.setInt("maxdepth", 64);
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        const GeometryPtr shapes = firstInput(in);
        std::string error;
        const auto g = compiled(error);
        if (!g) {
            setError(error);
            return shapes ? shapes : std::make_shared<Geometry>();
        }

        DeriveStats stats;
        const int maxDepth = std::max(1, params_.getInt("maxdepth", 64));
        GeometryPtr out = g->derive(shapes, maxDepth, &stats);
        if (stats.unfinished > 0) {
            setError("stopped after " + std::to_string(stats.passes) + " passes with " +
                     std::to_string(stats.unfinished) +
                     " shapes still to rewrite -- is a rule recursive?");
        } else {
            setError({});
        }
        return out;
    }

private:
    /// Parses on demand, and again only when the text changes. Handed out as a
    /// shared_ptr so a cook in flight keeps its grammar alive through an edit.
    std::shared_ptr<const Grammar> compiled(std::string& error) const {
        const std::string rules = params_.getString("rules", "");
        std::lock_guard<std::mutex> lk(mu_);
        if (!parsed_ || rules != source_) {
            source_ = rules;
            parseError_.clear();
            grammar_ = Grammar::parse(rules, parseError_);
            parsed_ = true;
        }
        error = parseError_;
        return grammar_;
    }

    mutable std::mutex mu_;
    mutable bool parsed_ = false;
    mutable std::string source_;
    mutable std::string parseError_;
    mutable std::shared_ptr<const Grammar> grammar_;
};

}  // namespace

void registerShapeNodes() {
    auto& r = NodeRegistry::instance();
    r.add("axiom", [](const std::string& n) { return std::make_unique<AxiomNode>(n); });
    r.add("lot", [](const std::string& n) { return std::make_unique<LotNode>(n); });
    r.add("extrude", [](const std::string& n) { return std::make_unique<ExtrudeNode>(n); });
    r.add("roof", [](const std::string& n) { return std::make_unique<RoofNode>(n); });
    r.add("split", [](const std::string& n) { return std::make_unique<SplitNode>(n); });
    r.add("comp", [](const std::string& n) { return std::make_unique<CompNode>(n); });
    r.add("shapemesh", [](const std::string& n) { return std::make_unique<ShapeMeshNode>(n); });
    r.add("shapegrammar", [](const std::string& n) { return std::make_unique<ShapeGrammarNode>(n); });
}

std::string grammarError(const Node& node) {
    const auto* g = dynamic_cast<const GrammarNode*>(&node);
    return g ? g->lastError() : std::string();
}

}  // namespace pg
