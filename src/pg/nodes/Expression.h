#pragma once
//
// A tiny per-element expression language -- the prototype's stand-in for the
// production wrangle language (ROADMAP.md M5).
//
// What it demonstrates:
//   * per-point execution bound to attributes by slot, resolved once per cook
//     rather than by name lookup per point;
//   * static type inference (float vs vector) deciding the type of attributes
//     the snippet creates;
//   * execution over deterministic chunks, so results do not depend on threads.
//
// What it deliberately is NOT: fast. It is a tree-walking interpreter, and it
// is here partly to be the baseline the JIT has to beat by 50x at M5. The
// binding seam (Program::run) is exactly where compiled kernels plug in.
//
// Grammar:
//   program := stmt (';' stmt)* ';'?
//   stmt    := '@' ident ('.' [xyz])? '=' expr
//   expr    := term (('+'|'-') term)*
//   term    := factor (('*'|'/') factor)*
//   factor  := ('-'|'+')? primary
//   primary := number | '@' ident ('.' [xyz])? | ident '(' args ')' | '(' expr ')'
//
// Bound names: @P, @ptnum, @numpt, @Time, @Frame, plus any point attribute.
// Builtins: sin cos abs sqrt floor pow min max clamp length noise fit vec3
//
#include "pg/core/Geometry.h"
#include "pg/core/Node.h"

#include <memory>
#include <string>
#include <vector>

namespace pg::expr {

/// A runtime value: float or vector. Binary operators promote float to vector
/// componentwise.
struct Value {
    bool isVec = false;
    float f = 0.0f;
    Vec3 v;

    static Value scalar(float x) { return Value{false, x, Vec3(x)}; }
    static Value vector(const Vec3& x) { return Value{true, x.x, x}; }
    Vec3 asVec() const { return isVec ? v : Vec3(f); }
    float asFloat() const { return isVec ? v.x : f; }
};

class Program {
public:
    ~Program();

    /// Parses `source`. Returns nullptr and fills `error` on failure.
    static std::unique_ptr<Program> parse(const std::string& source, std::string& error);

    /// Runs the program over every point of `geo`, creating attributes the
    /// snippet writes to. Returns false and fills `error` on a binding problem
    /// (for example writing a component of a non-vector attribute).
    bool run(Geometry& geo, const CookContext& ctx, std::string& error) const;

    /// Distinct `@name`s referenced, in first-seen order. One slot each.
    const std::vector<std::string>& slotNames() const;

    struct Impl;

private:
    Program();
    std::unique_ptr<Impl> impl_;
};

}  // namespace pg::expr
