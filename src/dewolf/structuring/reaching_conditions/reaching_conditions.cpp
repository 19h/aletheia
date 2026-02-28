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
    if (!graph_slice || !src) {
        return reaching_conditions;
    }

    // Start with True for the source node
    reaching_conditions.insert({src, dewolf_logic::LogicCondition(ctx.bool_val(true))});

    // Topological sort of the slice
    std::vector<TransitionBlock*> topo_order;
    std::unordered_map<TransitionBlock*, int> in_degree;

    for (TransitionBlock* node : graph_slice->blocks()) {
        if (!node) {
            continue;
        }
        in_degree[node] = 0;
    }

    for (TransitionBlock* node : graph_slice->blocks()) {
        if (!node) {
            continue;
        }
        for (TransitionEdge* edge : node->successors()) {
            if (!edge) {
                continue;
            }
            TransitionBlock* succ = edge->sink();
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
        if (!u) {
            continue;
        }
        topo_order.push_back(u);

        for (TransitionEdge* edge : u->successors()) {
            if (!edge) {
                continue;
            }
            TransitionBlock* v = edge->sink();
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
        if (!node || node == src) continue;

        z3::expr node_cond = ctx.bool_val(false);
        bool has_preds = false;

        for (TransitionEdge* edge : node->predecessors()) {
            if (!edge) {
                continue;
            }
            TransitionBlock* pred = edge->source();
            if (reaching_conditions.contains(pred)) {
                // The edge tag holds the condition!
                z3::expr edge_cond = edge->tag().expression();
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
