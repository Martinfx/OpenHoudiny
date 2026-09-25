#pragma once
#include "pg/core/Node.h"

#include <string>

namespace pg {

void registerGeneratorNodes();
void registerModifierNodes();
void registerWrangleNodes();
void registerShapeNodes();

/// Parse/run error of a `pointwrangle` node; empty if it is fine.
std::string wrangleError(const Node& node);

/// Last problem of a `split` or `shapegrammar` node -- a syntax error, or a
/// derivation that hit `maxdepth`; empty if it is fine.
std::string grammarError(const Node& node);

}  // namespace pg
