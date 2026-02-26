#include "reaching_conditions.hpp"
#include <queue>
#include <unordered_set>

namespace dewolf {

std::unordered_map<TransitionBlock*, dewolf_logic::LogicCondition> ReachingConditions::compute(
    z3::context& ctx,
    TransitionCFG* graph_slice,
    TransitionBlock* src,
    TransitionCFG* original_cfg
) {
    std::unordered_map<TransitionBlock*, dewolf_logic::LogicCondition> reaching_conditions;

    // Start with True for the source node
    reaching_conditions.insert({src, dewolf_logic::LogicCondition(ctx.bool_val(true))});

    // Topological sort of the slice
    std::vector<TransitionBlock*> topo_order;
    std::unordered_map<TransitionBlock*, int> in_degree;

    for (TransitionBlock* node : graph_slice->blocks()) {
        in_degree[node] = 0;
    }

    for (TransitionBlock* node : graph_slice->blocks()) {
        for (TransitionBlock* succ : node->successors()) {
            if (in_degree.contains(succ)) {
                in_degree[succ]++;
            }
        }
    }

    std::queue<TransitionBlock*> q;
    for (auto& [node, deg] : in_degree) {
        if (deg == 0) q.push(node);
    }

    while (!q.empty()) {
        TransitionBlock* u = q.front();
        q.pop();
        topo_order.push_back(u);

        for (TransitionBlock* v : u->successors()) {
            if (in_degree.contains(v)) {
                in_degree[v]--;
                if (in_degree[v] == 0) {
                    q.push(v);
                }
            }
        }
    }

    // Propagate conditions
    for (TransitionBlock* node : topo_order) {
        if (node == src) continue;

        z3::expr node_cond = ctx.bool_val(false);
        bool has_preds = false;

        for (TransitionBlock* pred : node->predecessors()) {
            if (reaching_conditions.contains(pred)) {
                z3::expr edge_cond = ctx.bool_val(true);

                BasicBlock* orig_pred = pred->ast_node()->get_original_block();
                BasicBlock* orig_node = node->ast_node()->get_original_block();

                if (orig_pred && orig_node) {
                    for (Edge* e : orig_pred->successors()) {
                        if (e->target() == orig_node) {
                            if (e->type() == EdgeType::True || e->type() == EdgeType::False) {
                                // Extract the condition from the predecessor's last instruction
                                if (!orig_pred->instructions().empty()) {
                                    Instruction* last_inst = orig_pred->instructions().back();
                                    
                                    // Use the new Branch type to extract the condition
                                    if (auto* branch = dynamic_cast<Branch*>(last_inst)) {
                                        dewolf_logic::Z3Converter z3_conv(ctx);
                                        dewolf_logic::LogicCondition c = z3_conv.convert_to_condition(branch->condition());
                                        if (e->type() == EdgeType::True) {
                                            edge_cond = c.expression();
                                        } else {
                                            edge_cond = (!c.expression());
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                node_cond = node_cond || (reaching_conditions.at(pred).expression() && edge_cond);
                has_preds = true;
            }
        }

        if (has_preds) {
            reaching_conditions.insert({node, dewolf_logic::LogicCondition(node_cond).simplify()});
        }
    }

    return reaching_conditions;
}

} // namespace dewolf
