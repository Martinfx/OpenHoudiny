#pragma once
//
// The checker (Check.cpp): the parse, typed for one run.
//
#include "pg/lang/Ast.h"

#include <string>
#include <vector>

namespace pg::lang {

Type fromAttr(AttrType t);
bool toAttr(Type t, AttrType& out);

struct Checked {
    std::vector<TFunction> functions;
    std::vector<TSPtr> main;
    /// Runs in order, one element after another: it makes or deletes
    /// geometry, writes other elements or strings.
    bool ordered = false;
    bool needVertexPrim = false;
    bool needCentroids = false;
    /// An expression: its type, and where its value goes.
    Type exprType = Type::Void;
    int exprSlot = -1;
    std::vector<std::string> warnings;
};

/// Types `ast` for a run over `geo` -- its attribute types, its inputs in
/// run.inputs -- or, with `expression`, for an expression without geometry.
/// `fold`: $F and ch("name") are the same for the whole run (run.host): they
/// become constants. False, with why, if the program does not type.
bool check(const Ast& ast, Run& run, Geometry* geo, bool expression, bool fold, Checked& out, std::string& error);

/// Makes what the bindings of `run` need in `geo` -- the attributes and
/// groups written that are not there -- and points them at the data.
bool bindAll(Run& run, Geometry& geo, const Checked& checked, std::string& error);

// What the checker hands the builtins it types itself (Builtins.cpp).
Impl implHostVariable(Type t);              ///< $name, read at the run
Impl implChannel(Type t);                   ///< ch(path), read at the run
Impl implAttribRead(Type t, bool resolved); ///< point(), prim(), vertex(), detail()
Impl implFormat(int kind);                  ///< 0 printf, 1 sprintf, 2 warning, 3 error

}  // namespace pg::lang
