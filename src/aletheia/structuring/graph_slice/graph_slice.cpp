#include "graph_slice.hpp"
#include <algorithm>

namespace aletheia {

std::vector<TransitionBlock*> GraphSlice::get_sink_nodes(TransitionCFG* t_cfg, const std::unordered_set<TransitionBlock*>& region) {
    std::vector<TransitionBlock*> sinks;
    for (TransitionBlock* node : region) {
        if (!node) {
            continue;
        }
        if (node->successors_blocks().empty()) {
            sinks.push_back(node);
            continue;
        }
        for (TransitionBlock* succ : node->successors_blocks()) {
            if (!succ) {
                continue;
            }
            if (region.find(succ) == region.end()) {
                sinks.push_back(node);
                break;
            }
        }
    }
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
    std::unordered_set<TransitionBlock*> slice_nodes;
    std::unordered_set<TransitionBlock*> path;
    std::unordered_set<TransitionBlock*> visited;

    auto dfs = [&](TransitionBlock* node, auto& dfs_ref) -> bool {
        if (!node) return false;
        
        path.insert(node);
        visited.insert(node);

        bool reaches_sink = false;
        if (std::find(sink_nodes.begin(), sink_nodes.end(), node) != sink_nodes.end()) {
            reaches_sink = true;
        }
        for (TransitionBlock* succ : node->successors_blocks()) {
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

    for (TransitionBlock* node : slice_nodes) {
        if (node) {
            slice->add_block(node);
        }
    }

    return slice;
}

} // namespace aletheia
