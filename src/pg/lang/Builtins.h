#pragma once
//
// Registering builtins (Builtins.cpp, BuiltinsGeo.cpp).
//
#include "pg/lang/Ast.h"

namespace pg::lang {

/// Adds an overload of `name`. `refs`: how many leading arguments are
/// variables it writes into; `sideEffect`: it changes geometry, so the
/// program runs in order; `variadic`: the last parameter repeats.
void add(Builtins& b, const char* name, Type ret, std::vector<Type> params, Impl fn, int refs = 0,
         bool sideEffect = false, bool variadic = false);

void addMathBuiltins(Builtins& b);
void addGeometryBuiltins(Builtins& b);

}  // namespace pg::lang
