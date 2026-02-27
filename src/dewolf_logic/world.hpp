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

    DagNode* map_condition(DagNode* condition);

    LogicDag& dag() { return dag_; }

private:
    DagNode* map_condition_impl(DagNode* condition);

    LogicDag dag_;
    std::unordered_map<const DagNode*, DagNode*> mapped_nodes_;
};

} // namespace dewolf_logic
