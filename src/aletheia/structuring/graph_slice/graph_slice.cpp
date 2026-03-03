#include "graph_slice.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

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

std::vector<TransitionBlock*> sorted_transition_blocks(const std::vector<TransitionBlock*>& blocks) {
    std::vector<TransitionBlock*> sorted = blocks;
    sort_transition_blocks_deterministically(sorted);
    return sorted;
}

} // namespace

std::vector<TransitionBlock*> GraphSlice::get_sink_nodes(TransitionCFG* t_cfg, const std::unordered_set<TransitionBlock*>& region) {
    std::vector<TransitionBlock*> sinks;
    std::vector<TransitionBlock*> region_nodes(region.begin(), region.end());
    sort_transition_blocks_deterministically(region_nodes);

    for (TransitionBlock* node : region_nodes) {
        if (!node) {
            continue;
        }
        if (node->successors_blocks().empty()) {
            sinks.push_back(node);
            continue;
        }
        for (TransitionBlock* succ : sorted_transition_blocks(node->successors_blocks())) {
            if (!succ) {
                continue;
            }
            if (region.find(succ) == region.end()) {
                sinks.push_back(node);
                break;
            }
        }
    }
    sort_transition_blocks_deterministically(sinks);
    sinks.erase(std::unique(sinks.begin(), sinks.end()), sinks.end());
    return sinks;
}

TransitionCFG* GraphSlice::compute_graph_slice_for_region(
    DecompilerArena& arena,
    TransitionCFG* t_cfg,
    TransitionBlock* source,
    const std::unordered_set<TransitionBlock*>& region,
    bool back_edges
) {
    auto sink_nodes = get_sink_nodes(t_cfg, region);
    return compute_graph_slice_for_sink_nodes(arena, t_cfg, source, sink_nodes, back_edges);
}

TransitionCFG* GraphSlice::compute_graph_slice_for_sink_nodes(
    DecompilerArena& arena,
    TransitionCFG* t_cfg,
    TransitionBlock* source,
    const std::vector<TransitionBlock*>& sink_nodes,
    bool back_edges
) {
    // Port of GraphSlice logic:
    // A slice is a directed acyclic subgraph where N is the set of all vertices on simple paths
    // from source to any of the sink_nodes.

    TransitionCFG* slice = arena.create<TransitionCFG>(arena);
    slice->set_entry(source);
    if (!source) {
        return slice;
    }

    // Simple implementation: DFS from source. If path reaches a sink, all nodes in path are in the slice.
    std::vector<TransitionBlock*> sorted_sinks = sink_nodes;
    sort_transition_blocks_deterministically(sorted_sinks);

    std::unordered_set<TransitionBlock*> sink_set(sorted_sinks.begin(), sorted_sinks.end());
    std::unordered_set<TransitionBlock*> slice_nodes;
    std::unordered_set<TransitionBlock*> path;
    std::unordered_set<TransitionBlock*> visited;

    auto dfs = [&](TransitionBlock* node, auto& dfs_ref) -> bool {
        if (!node) return false;
        
        path.insert(node);
        visited.insert(node);

        bool reaches_sink = false;
        if (sink_set.contains(node)) {
            reaches_sink = true;
        }
        for (TransitionBlock* succ : sorted_transition_blocks(node->successors_blocks())) {
                if (!succ) {
                    continue;
                }
                if (path.contains(succ)) {
                    // Back-edge
                    if (back_edges) reaches_sink = true; // Loosely keeping back edges
                    continue;
                }
                
                // If it was already processed and known to be in slice, it reaches sink
                if (slice_nodes.contains(succ)) {
                    reaches_sink = true;
                } else if (!visited.contains(succ)) {
                    if (dfs_ref(succ, dfs_ref)) {
                        reaches_sink = true;
                    }
                }
            }

        path.erase(node);
        
        if (reaches_sink) {
            slice_nodes.insert(node);
        }
        return reaches_sink;
    };

    dfs(source, dfs);

    std::vector<TransitionBlock*> ordered_slice_nodes(slice_nodes.begin(), slice_nodes.end());
    sort_transition_blocks_deterministically(ordered_slice_nodes);
    for (TransitionBlock* node : ordered_slice_nodes) {
        if (node) {
            slice->add_block(node);
        }
    }

    return slice;
}

} // namespace aletheia
