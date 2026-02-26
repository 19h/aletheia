#include "cfg.hpp"
#include <unordered_set>
#include <algorithm>

namespace dewolf {

static void dfs_post_order(BasicBlock* block, std::unordered_set<std::size_t>& visited, std::vector<BasicBlock*>& result) {
    if (!block) return;
    
    visited.insert(block->id());
    
    for (Edge* edge : block->successors()) {
        BasicBlock* target = edge->target();
        if (target && visited.find(target->id()) == visited.end()) {
            dfs_post_order(target, visited, result);
        }
    }
    
    result.push_back(block);
}

static void dfs_pre_order(BasicBlock* block, std::unordered_set<std::size_t>& visited, std::vector<BasicBlock*>& result) {
    if (!block) return;
    
    visited.insert(block->id());
    result.push_back(block);
    
    for (Edge* edge : block->successors()) {
        BasicBlock* target = edge->target();
        if (target && visited.find(target->id()) == visited.end()) {
            dfs_pre_order(target, visited, result);
        }
    }
}

std::vector<BasicBlock*> ControlFlowGraph::post_order() const {
    std::vector<BasicBlock*> result;
    std::unordered_set<std::size_t> visited;
    
    if (entry_block_) {
        dfs_post_order(entry_block_, visited, result);
    }
    
    // In case of disconnected components
    for (BasicBlock* block : blocks_) {
        if (visited.find(block->id()) == visited.end()) {
            dfs_post_order(block, visited, result);
        }
    }
    
    return result;
}

std::vector<BasicBlock*> ControlFlowGraph::reverse_post_order() const {
    std::vector<BasicBlock*> result = post_order();
    std::reverse(result.begin(), result.end());
    return result;
}

std::vector<BasicBlock*> ControlFlowGraph::dfs() const {
    std::vector<BasicBlock*> result;
    std::unordered_set<std::size_t> visited;
    
    if (entry_block_) {
        dfs_pre_order(entry_block_, visited, result);
    }
    
    for (BasicBlock* block : blocks_) {
        if (visited.find(block->id()) == visited.end()) {
            dfs_pre_order(block, visited, result);
        }
    }
    
    return result;
}

void ControlFlowGraph::remove_edge(Edge* edge) {
    if (!edge) return;
    if (edge->source()) {
        edge->source()->remove_successor(edge);
    }
    if (edge->target()) {
        edge->target()->remove_predecessor(edge);
    }
}

void ControlFlowGraph::substitute_edge(Edge* old_edge, Edge* new_edge) {
    if (!old_edge || !new_edge) return;
    if (old_edge->source()) {
        old_edge->source()->substitute_successor(old_edge, new_edge);
    }
    if (old_edge->target()) {
        old_edge->target()->remove_predecessor(old_edge);
    }
    if (new_edge->target()) {
        new_edge->target()->add_predecessor(new_edge);
    }
}

void ControlFlowGraph::remove_nodes_from(const std::unordered_set<BasicBlock*>& dead_blocks) {
    // 1. Remove all edges connected to dead blocks
    for (BasicBlock* block : dead_blocks) {
        // Collect edges to avoid modifying while iterating
        std::vector<Edge*> succs = block->successors();
        for (Edge* edge : succs) {
            remove_edge(edge);
        }
        std::vector<Edge*> preds = block->predecessors();
        for (Edge* edge : preds) {
            remove_edge(edge);
        }
    }

    // 2. Remove dead blocks from the graph's block list
    std::erase_if(blocks_, [&dead_blocks](BasicBlock* block) {
        return dead_blocks.count(block) > 0;
    });

    // 3. Clear entry block if it's dead
    if (entry_block_ && dead_blocks.count(entry_block_) > 0) {
        entry_block_ = nullptr;
    }
}

} // namespace dewolf
