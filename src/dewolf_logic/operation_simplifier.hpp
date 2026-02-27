#pragma once

#include "dag.hpp"

namespace dewolf_logic {

// Simplify a logic DAG node recursively.
DagNode* simplify_node(LogicDag& dag, DagNode* node);

} // namespace dewolf_logic
