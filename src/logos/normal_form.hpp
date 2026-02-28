#pragma once

#include "dag.hpp"

namespace logos {

class ToCnfVisitor {
public:
    static DagNode* convert(LogicDag& dag, DagNode* root);
};

class ToDnfVisitor {
public:
    static DagNode* convert(LogicDag& dag, DagNode* root);
};

} // namespace logos
