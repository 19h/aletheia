#pragma once

#include "dag.hpp"

namespace logos {

// Simplify a logic DAG node recursively.
DagNode* simplify_node(LogicDag& dag, DagNode* node);

} // namespace logos
