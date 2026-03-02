#include "world.hpp"

namespace logos {

DagNode* World::map_condition(DagNode* condition) {
    if (condition == nullptr) {
        return nullptr;
    }
    return map_condition_impl(condition);
}

DagNode* World::map_condition_impl(DagNode* condition) {
    if (condition == nullptr) {
        return nullptr;
    }

    auto mapped = mapped_nodes_.find(condition);
    if (mapped != mapped_nodes_.end()) {
        return mapped->second;
    }

    DagNode* result = nullptr;

    if (auto* var = dag_dyn_cast<DagVariable>(condition)) {
        result = dag_.create_node<DagVariable>(var->name());
    } else if (auto* constant = dag_dyn_cast<DagConstant>(condition)) {
        result = dag_.create_node<DagConstant>(constant->value());
    } else if (auto* op = dag_dyn_cast<DagOperation>(condition)) {
        auto* op_copy = dag_.create_node<DagOperation>(op->op());
        // Insert the node in the map before descending so shared subgraphs and
        // recursive references are handled safely and without reallocation.
        mapped_nodes_[condition] = op_copy;
        for (DagNode* child : op->children()) {
            op_copy->add_child(map_condition_impl(child));
        }
        return op_copy;
    } else {
        // Conservative fallback for unknown node kinds: preserve printable
        // identity rather than dropping the subtree.
        result = dag_.create_node<DagVariable>(condition->to_string());
    }

    mapped_nodes_[condition] = result;
    return result;
}

} // namespace logos
