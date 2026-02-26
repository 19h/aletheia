#include "cfg.hpp"
#include "../ssa/dominators.hpp"
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <vector>

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


static bool has_tree_path(BasicBlock* start, BasicBlock* finish, const std::unordered_map<BasicBlock*, BasicBlock*>& parent_dict) {
    BasicBlock* current = start;
    while (parent_dict.find(current) != parent_dict.end()) {
        current = parent_dict.at(current);
        if (current == finish) {
            return true;
        }
    }
    return false;
}

void ControlFlowGraph::classify_edges(DominatorTree& dom_tree) {
    edge_properties_.clear();
    
    std::unordered_map<BasicBlock*, int> node_indices;
    std::unordered_map<BasicBlock*, BasicBlock*> parent_dict;
    
    int index = blocks_.size();
    std::unordered_set<BasicBlock*> visited_nodes;
    
    if (!entry_block_) return;
    
    visited_nodes.insert(entry_block_);
    
    // Stack holds <Parent, ChildIterator>
    // We emulate iterator by an integer index into successors
    std::vector<std::pair<BasicBlock*, size_t>> stack;
    stack.push_back({entry_block_, 0});
    
    while (!stack.empty()) {
        auto& top = stack.back();
        BasicBlock* parent = top.first;
        size_t& child_idx = top.second;
        
        if (child_idx < parent->successors().size()) {
            Edge* edge = parent->successors()[child_idx];
            BasicBlock* child = edge->target();
            child_idx++;
            
            if (visited_nodes.count(child)) {
                EdgeProperty prop;
                if (node_indices.count(child)) {
                    if (has_tree_path(child, parent, parent_dict)) {
                        prop = EdgeProperty::Forward;
                    } else {
                        prop = EdgeProperty::Cross;
                    }
                } else {
                    if (dom_tree.dominates(child, parent)) {
                        prop = EdgeProperty::Back;
                    } else {
                        prop = EdgeProperty::Retreating;
                    }
                }
                edge_properties_[edge] = prop;
            } else {
                edge_properties_[edge] = EdgeProperty::Tree;
                parent_dict[child] = parent;
                visited_nodes.insert(child);
                stack.push_back({child, 0});
            }
        } else {
            node_indices[parent] = index;
            index--;
            stack.pop_back();
        }
    }
}

std::unordered_map<BasicBlock*, std::unordered_set<Edge*>> ControlFlowGraph::back_edges() const {
    std::unordered_map<BasicBlock*, std::unordered_set<Edge*>> result;
    for (auto& [edge, prop] : edge_properties_) {
        if (prop == EdgeProperty::Back) {
            result[edge->target()].insert(edge);
        }
    }
    return result;
}

std::unordered_set<Edge*> ControlFlowGraph::retreating_edges() const {
    std::unordered_set<Edge*> result;
    for (auto& [edge, prop] : edge_properties_) {
        if (prop == EdgeProperty::Retreating) {
            result.insert(edge);
        }
    }
    return result;
}

} // namespace dewolf
