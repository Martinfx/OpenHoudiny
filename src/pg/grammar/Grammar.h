#pragma once
//
// The text front-end of the shape grammar: a small rule language in the style
// of CGA shape (Müller et al., "Procedural Modeling of Buildings", 2006), and
// the derivation that runs it.
//
//   // comments run to the end of the line, and so does '#'
//   Lot      --> extrude(height) Mass
//   Mass     --> comp(f) { side: Facade | top: RoofBase }
//   RoofBase --> roofHip(30) Roof
//   Facade   --> split(y) { 4: Ground | { ~3.2: Floor }* }
//   Floor    --> split(x) { 1: Wall | { ~3: Tile }* | 1: Wall }
//   Tile     --> split(x) { ~1: Wall | 1.2: Window | ~1: Wall }
//   Window   --> extrude(-0.2) Glass
//
// Syntax:
//   grammar   := rule*
//   rule      := Symbol ('-->' | '->') op* successor?
//   op        := ('extrude' | 'roofHip' | 'roofGable') '(' arg ')'
//   successor := Symbol | 'NIL'
//              | 'split' '(' axis ')' pattern
//              | 'comp' '(' 'f' ')' '{' face ':' Symbol ('|' face ':' Symbol)* '}'
//   pattern   := '{' item ('|' item)* '}' '*'?
//   item      := part | '{' part ('|' part)* '}' '*'
//   part      := size ':' Symbol
//   size      := number | '~' number | '\'' number
//   arg       := number | attribute-name
//   axis      := x | y | z
//   face      := front | back | left | right | side | top | bottom
//
// A rule rewrites every shape that carries its symbol: the ops run on the shape
// in order, then the successor names the result -- or replaces it with the
// pieces of a split or the faces of a comp. A rule with no successor leaves the
// shape under its old name, finished. A shape whose symbol has no rule is a
// terminal and stays as it is.
//
// An `arg` that is a name reads that point attribute of the shape being
// rewritten, so a pointwrangle upstream can give every lot its own height.
//
#include "pg/grammar/Shape.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pg::grammar {

/// Parses a split pattern: exactly what follows `split(axis)` in a rule, e.g.
/// "{ 4: Ground | { ~3.2: Floor }* }". The split node uses the same syntax.
bool parseSplitPattern(const std::string& text, SplitPattern& out, std::string& error);

/// Per-shape value of a float or int point attribute; `fallback` without one.
float attributeValue(const AttributeArray* attribute, size_t shape, float fallback);

/// A number, or the name of a point attribute read per shape.
struct Arg {
    float value = 0.0f;
    int attribute = -1;  ///< index into Grammar::attributeNames(), or -1 for `value`
};

enum class OpKind : uint8_t { Extrude, RoofHip, RoofGable };

struct Op {
    OpKind kind = OpKind::Extrude;
    Arg arg;
};

enum class SuccessorKind : uint8_t { None, Symbol, Nil, Split, Comp };

struct Rule {
    std::string predecessor;
    std::vector<Op> ops;
    SuccessorKind successor = SuccessorKind::None;
    std::string symbol;    ///< for SuccessorKind::Symbol
    int axis = 0;          ///< for SuccessorKind::Split
    SplitPattern pattern;  ///< for SuccessorKind::Split
    CompTargets comp;      ///< for SuccessorKind::Comp
    int line = 0;
};

struct DeriveStats {
    int passes = 0;         ///< rewriting passes that ran
    size_t unfinished = 0;  ///< shapes that still had a rule to apply when maxDepth hit
};

class Grammar {
public:
    /// Returns nullptr and fills `error` ("line 3, col 7: ...") on a syntax error.
    static std::unique_ptr<Grammar> parse(const std::string& source, std::string& error);

    const std::vector<Rule>& rules() const { return rules_; }
    /// The rule for `symbol`, or nullptr if the symbol is a terminal.
    const Rule* find(const std::string& symbol) const;
    const std::vector<std::string>& attributeNames() const { return attributes_; }

    /// Rewrites `shapes` pass by pass until no shape has a rule left to apply,
    /// or `maxDepth` passes have run -- the guard against recursive rules.
    GeometryPtr derive(const GeometryPtr& shapes, int maxDepth,
                       DeriveStats* stats = nullptr) const;

private:
    void apply(const Rule& rule, uint32_t parent, Shape shape,
               const std::vector<const AttributeArray*>& attributes,
               std::vector<Successor>& out) const;

    std::vector<Rule> rules_;
    std::unordered_map<std::string, size_t> index_;
    std::vector<std::string> attributes_;
};

}  // namespace pg::grammar
