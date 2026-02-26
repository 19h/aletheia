#pragma once
#include "dag.hpp"
#include <memory>
#include <unordered_map>
#include <vector>

namespace dewolf_logic {

class WorldObject {
public:
    virtual ~WorldObject() = default;
};

class World {
public:
    World() = default;

    DagNode* map_condition(DagNode* condition) {
        // Here we would transform external AST conditions to logic DAG
        // For now, it just returns the condition if it's already a DagNode
        return condition;
    }

    LogicDag& dag() { return dag_; }

private:
    LogicDag dag_;
};

} // namespace dewolf_logic
