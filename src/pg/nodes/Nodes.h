#pragma once
#include "pg/core/Node.h"

#include <string>

namespace pg {

void registerGeneratorNodes();
void registerModifierNodes();
void registerWrangleNodes();

/// Parse/run error of a `pointwrangle` node; empty if it is fine.
std::string wrangleError(const Node& node);

}  // namespace pg
