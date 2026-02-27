#pragma once

#include "dag.hpp"

namespace dewolf_logic {

class ToCnfVisitor {
public:
    static DagNode* convert(LogicDag& dag, DagNode* root);
};

class ToDnfVisitor {
public:
    static DagNode* convert(LogicDag& dag, DagNode* root);
};

} // namespace dewolf_logic
