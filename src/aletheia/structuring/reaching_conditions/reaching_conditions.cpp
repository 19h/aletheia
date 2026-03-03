#include "reaching_conditions.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <string>
#include <unordered_set>

namespace aletheia {

namespace {

void collect_original_block_ids(AstNode* node, std::vector<std::uint64_t>& ids) {
    if (!node) {
        return;
    }

    if (BasicBlock* bb = node->get_original_block()) {
        ids.push_back(static_cast<std::uint64_t>(bb->id()));
    }

    if (auto* seq = ast_dyn_cast<SeqNode>(node)) {
        for (AstNode* child : seq->nodes()) {
            collect_original_block_ids(child, ids);
        }
        return;
    }
    if (auto* if_node = ast_dyn_cast<IfNode>(node)) {
        collect_original_block_ids(if_node->true_branch(), ids);
        collect_original_block_ids(if_node->false_branch(), ids);
        return;
    }
    if (auto* loop = ast_dyn_cast<LoopNode>(node)) {
        collect_original_block_ids(loop->body(), ids);
        return;
    }
    if (auto* sw = ast_dyn_cast<SwitchNode>(node)) {
        for (CaseNode* case_node : sw->cases()) {
            collect_original_block_ids(case_node->body(), ids);
        }
        return;
    }
    if (auto* case_node = ast_dyn_cast<CaseNode>(node)) {
        collect_original_block_ids(case_node->body(), ids);
    }
}

std::uint64_t transition_block_order_key(TransitionBlock* node) {
    if (!node || !node->ast_node()) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    if (BasicBlock* bb = node->ast_node()->get_original_block()) {
        return static_cast<std::uint64_t>(bb->id());
    }

    std::vector<std::uint64_t> ids;
    collect_original_block_ids(node->ast_node(), ids);
    if (!ids.empty()) {
        return *std::min_element(ids.begin(), ids.end());
    }

    const auto kind_bias = static_cast<std::uint64_t>(node->ast_node()->ast_kind());
    return std::numeric_limits<std::uint64_t>::max() - kind_bias;
}

std::string transition_block_signature(TransitionBlock* node) {
    if (!node || !node->ast_node()) {
        return "<null>";
    }

    std::vector<std::uint64_t> ids;
    collect_original_block_ids(node->ast_node(), ids);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    std::string out = "k" + std::to_string(static_cast<int>(node->ast_node()->ast_kind())) + ":";
    if (ids.empty()) {
        out += "none";
    } else {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i != 0) {
                out += ",";
            }
            out += std::to_string(ids[i]);
        }
    }
    return out;
}

void sort_transition_blocks_deterministically(std::vector<TransitionBlock*>& blocks) {
    std::stable_sort(blocks.begin(), blocks.end(), [](TransitionBlock* lhs, TransitionBlock* rhs) {
        const std::uint64_t lhs_key = transition_block_order_key(lhs);
        const std::uint64_t rhs_key = transition_block_order_key(rhs);
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key;
        }
        return transition_block_signature(lhs) < transition_block_signature(rhs);
    });
}

std::vector<TransitionEdge*> sorted_successor_edges(TransitionBlock* node) {
    if (!node) {
        return {};
    }
    std::vector<TransitionEdge*> edges = node->successors();
    std::stable_sort(edges.begin(), edges.end(), [](TransitionEdge* lhs, TransitionEdge* rhs) {
        TransitionBlock* lhs_sink = lhs ? lhs->sink() : nullptr;
        TransitionBlock* rhs_sink = rhs ? rhs->sink() : nullptr;
        const std::uint64_t lhs_key = transition_block_order_key(lhs_sink);
        const std::uint64_t rhs_key = transition_block_order_key(rhs_sink);
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key;
        }
        return transition_block_signature(lhs_sink) < transition_block_signature(rhs_sink);
    });
    return edges;
}

std::vector<TransitionEdge*> sorted_predecessor_edges(TransitionBlock* node) {
    if (!node) {
        return {};
    }
    std::vector<TransitionEdge*> edges = node->predecessors();
    std::stable_sort(edges.begin(), edges.end(), [](TransitionEdge* lhs, TransitionEdge* rhs) {
        TransitionBlock* lhs_src = lhs ? lhs->source() : nullptr;
        TransitionBlock* rhs_src = rhs ? rhs->source() : nullptr;
        const std::uint64_t lhs_key = transition_block_order_key(lhs_src);
        const std::uint64_t rhs_key = transition_block_order_key(rhs_src);
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key;
        }
        return transition_block_signature(lhs_src) < transition_block_signature(rhs_src);
    });
    return edges;
}

} // namespace

std::unordered_map<TransitionBlock*, logos::LogicCondition> ReachingConditions::compute(
    z3::context& ctx,
    TransitionCFG* graph_slice,
    TransitionBlock* src,
    TransitionCFG* original_cfg
) {
    std::unordered_map<TransitionBlock*, logos::LogicCondition> reaching_conditions;
    if (!graph_slice || !src) {
        return reaching_conditions;
    }

    // Start with True for the source node
    reaching_conditions.insert({src, logos::LogicCondition(ctx.bool_val(true))});

    // Topological sort of the slice
    std::vector<TransitionBlock*> topo_order;
    std::unordered_map<TransitionBlock*, int> in_degree;

    std::vector<TransitionBlock*> ordered_blocks = graph_slice->blocks();
    sort_transition_blocks_deterministically(ordered_blocks);

    for (TransitionBlock* node : ordered_blocks) {
        if (!node) {
            continue;
        }
        in_degree[node] = 0;
    }

    for (TransitionBlock* node : ordered_blocks) {
        if (!node) {
            continue;
        }
        for (TransitionEdge* edge : sorted_successor_edges(node)) {
            if (!edge) {
                continue;
            }
            TransitionBlock* succ = edge->sink();
            if (in_degree.contains(succ)) {
                in_degree[succ]++;
            }
        }
    }

    std::vector<TransitionBlock*> ready;
    for (TransitionBlock* node : ordered_blocks) {
        auto it = in_degree.find(node);
        if (it != in_degree.end() && it->second == 0) {
            ready.push_back(node);
        }
    }
    sort_transition_blocks_deterministically(ready);

    while (!ready.empty()) {
        TransitionBlock* u = ready.front();
        ready.erase(ready.begin());
        if (!u) {
            continue;
        }
        topo_order.push_back(u);

        for (TransitionEdge* edge : sorted_successor_edges(u)) {
            if (!edge) {
                continue;
            }
            TransitionBlock* v = edge->sink();
            if (in_degree.contains(v)) {
                in_degree[v]--;
                if (in_degree[v] == 0) {
                    ready.push_back(v);
                }
            }
        }
        sort_transition_blocks_deterministically(ready);
    }

    // Propagate conditions
    for (TransitionBlock* node : topo_order) {
        if (!node || node == src) continue;

        z3::expr node_cond = ctx.bool_val(false);
        bool has_preds = false;

        for (TransitionEdge* edge : sorted_predecessor_edges(node)) {
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
            reaching_conditions.insert({node, logos::LogicCondition(node_cond).simplify()});
        }
    }

    return reaching_conditions;
}

} // namespace aletheia
