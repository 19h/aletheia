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

} // namespace dewolf
