#pragma once
#include "../../common/arena_allocated.hpp"
#include "ast.hpp"
#include "../../logos/z3_logic.hpp"
#include "../structures/cfg.hpp" // For EdgeProperty
#include <vector>
#include <algorithm>
#include <optional>

namespace aletheia {

class TransitionBlock;

class TransitionEdge : public ArenaAllocated {
public:
    TransitionEdge(TransitionBlock* source, TransitionBlock* sink, logos::LogicCondition tag, std::optional<EdgeProperty> property = std::nullopt)
        : source_(source), sink_(sink), tag_(tag), property_(property) {}

    TransitionBlock* source() const { return source_; }
    TransitionBlock* sink() const { return sink_; }
    logos::LogicCondition tag() const { return tag_; }
    std::optional<EdgeProperty> property() const { return property_; }

    void set_source(TransitionBlock* b) { source_ = b; }
    void set_sink(TransitionBlock* b) { sink_ = b; }
    void set_tag(logos::LogicCondition t) { tag_ = t; }
    void set_property(EdgeProperty p) { property_ = p; }

private:
    TransitionBlock* source_;
    TransitionBlock* sink_;
    logos::LogicCondition tag_;
    std::optional<EdgeProperty> property_;
};

class TransitionBlock : public ArenaAllocated {
public:
    explicit TransitionBlock(AstNode* ast_node) : ast_node_(ast_node) {}

    AstNode* ast_node() const { return ast_node_; }
    void set_ast_node(AstNode* node) { ast_node_ = node; }

    void add_predecessor_edge(TransitionEdge* e) { predecessors_.push_back(e); }
    void add_successor_edge(TransitionEdge* e) { successors_.push_back(e); }

    void remove_predecessor_edge(TransitionEdge* e) {
        predecessors_.erase(std::remove(predecessors_.begin(), predecessors_.end(), e), predecessors_.end());
    }

    void remove_successor_edge(TransitionEdge* e) {
        successors_.erase(std::remove(successors_.begin(), successors_.end(), e), successors_.end());
    }

    const std::vector<TransitionEdge*>& predecessors() const { return predecessors_; }
    const std::vector<TransitionEdge*>& successors() const { return successors_; }

    // Compatibility methods
    std::vector<TransitionBlock*> predecessors_blocks() const {
        std::vector<TransitionBlock*> res;
        for (auto* e : predecessors_) {
            if (e && e->source()) {
                res.push_back(e->source());
            }
        }
        return res;
    }

    std::vector<TransitionBlock*> successors_blocks() const {
        std::vector<TransitionBlock*> res;
        for (auto* e : successors_) {
            if (e && e->sink()) {
                res.push_back(e->sink());
            }
        }
        return res;
    }

    // These modify the underlying CFG, but we don't have edge context here!
    // They are used in places where we just add/remove block links without edge tags.
    // We should probably NOT have them and update callers to use TransitionCFG::add_edge.
    // However, to keep it building for now we can create synthetic edges.
    // Wait, TransitionBlock doesn't have an arena to create new edges.
    // Let's remove add_successor_block and force using CFG methods!

private:
    AstNode* ast_node_;
    std::vector<TransitionEdge*> predecessors_;
    std::vector<TransitionEdge*> successors_;
};

class TransitionCFG : public ArenaAllocated {
public:
    TransitionCFG(DecompilerArena& arena) : arena_(arena) {}

    void set_entry(TransitionBlock* entry) { entry_ = entry; }
    TransitionBlock* entry() const { return entry_; }

    void add_block(TransitionBlock* block) {
        if (!block) {
            return;
        }
        blocks_.push_back(block);
        domtree_valid_ = false;
    }
    
    void remove_block(TransitionBlock* block) {
        blocks_.erase(std::remove(blocks_.begin(), blocks_.end(), block), blocks_.end());
        if (entry_ == block) entry_ = nullptr;
        domtree_valid_ = false;
    }

    const std::vector<TransitionBlock*>& blocks() const { return blocks_; }

    // Edge management
    TransitionEdge* add_edge(TransitionBlock* src, TransitionBlock* dst, logos::LogicCondition tag, std::optional<EdgeProperty> prop = std::nullopt) {
        if (!src || !dst) {
            return nullptr;
        }
        TransitionEdge* edge = arena_.create<TransitionEdge>(src, dst, tag, prop);
        src->add_successor_edge(edge);
        dst->add_predecessor_edge(edge);
        domtree_valid_ = false;
        return edge;
    }

    
    void remove_edge_between(TransitionBlock* src, TransitionBlock* dst) {
        if (!src || !dst) {
            return;
        }
        TransitionEdge* to_remove = nullptr;
        for (auto* e : src->successors()) {
            if (e->sink() == dst) {
                to_remove = e;
                break;
            }
        }
        if (to_remove) remove_edge(to_remove);
    }

    void remove_edge(TransitionEdge* edge) {
        if (!edge || !edge->source() || !edge->sink()) {
            return;
        }
        edge->source()->remove_successor_edge(edge);
        edge->sink()->remove_predecessor_edge(edge);
        domtree_valid_ = false;
    }

    // Utility methods
    std::vector<TransitionEdge*> get_in_edges(TransitionBlock* block) const {
        if (!block) {
            return {};
        }
        return block->predecessors();
    }

    std::vector<TransitionEdge*> get_out_edges(TransitionBlock* block) const {
        if (!block) {
            return {};
        }
        return block->successors();
    }


    void refresh_edge_properties() {
        std::unordered_map<TransitionBlock*, int> node_indices;
        std::unordered_map<TransitionBlock*, TransitionBlock*> parent_dict;
        int index = blocks_.size();
        std::unordered_set<TransitionBlock*> visited_nodes;
        
        if (!entry_) return;
        
        std::vector<std::pair<TransitionBlock*, size_t>> stack;
        stack.push_back({entry_, 0});
        visited_nodes.insert(entry_);
        
        while (!stack.empty()) {
            auto& top = stack.back();
            TransitionBlock* parent = top.first;
            auto& child_idx = top.second;
            if (!parent) {
                stack.pop_back();
                continue;
            }

            auto edges = parent->successors();
            if (child_idx < edges.size()) {
                TransitionEdge* edge = edges[child_idx++];
                if (!edge) {
                    continue;
                }
                TransitionBlock* child = edge->sink();
                if (!child) {
                    edge->set_property(EdgeProperty::NonLoop);
                    continue;
                }
                
                if (visited_nodes.contains(child)) {
                    EdgeProperty prop = EdgeProperty::NonLoop;
                    if (node_indices.contains(child)) {
                        // Forward or cross, we don't care, both map to NonLoop
                        prop = EdgeProperty::NonLoop;
                    } else {
                        // Back or retreating
                        if (dominates(child, parent)) {
                            prop = EdgeProperty::Back;
                        } else {
                            prop = EdgeProperty::Retreating;
                        }
                    }
                    edge->set_property(prop);
                } else {
                    edge->set_property(EdgeProperty::NonLoop);
                    parent_dict[child] = parent;
                    visited_nodes.insert(child);
                    stack.push_back({child, 0});
                }
            } else {
                node_indices[parent] = index--;
                stack.pop_back();
            }
        }
    }


    std::vector<TransitionBlock*> get_loop_heads() const {
        std::vector<TransitionBlock*> post_order;
        std::unordered_set<TransitionBlock*> visited;
        auto dfs = [&](TransitionBlock* node, auto& dfs_ref) -> void {
            if (!node || visited.contains(node)) return;
            visited.insert(node);
            for (auto* succ : node->successors_blocks()) dfs_ref(succ, dfs_ref);
            post_order.push_back(node);
        };
        dfs(entry_, dfs);
        
        std::vector<TransitionBlock*> loop_heads;
        for (auto* node : post_order) {
            if (!node) {
                continue;
            }
            for (auto* e : node->predecessors()) {
                if (!e) {
                    continue;
                }
                if (e->property() == EdgeProperty::Back || e->property() == EdgeProperty::Retreating) {
                    loop_heads.push_back(node);
                    break;
                }
            }
        }
        return loop_heads;
    }

    /// Compute or refresh the cached dominator tree (Cooper et al. algorithm).
    /// Called automatically on first `dominates()` query. Call `invalidate_domtree()`
    /// after modifying the CFG structure (adding/removing blocks or edges).
    void compute_domtree() const {
        idom_.clear();
        rpo_.clear();
        rpo_index_.clear();
        if (!entry_) return;

        // 1. Compute reverse post-order (RPO) via iterative DFS.
        std::unordered_set<TransitionBlock*> visited;
        std::vector<TransitionBlock*> post_order;
        std::vector<std::pair<TransitionBlock*, std::size_t>> stk;
        stk.push_back({entry_, 0});
        visited.insert(entry_);
        while (!stk.empty()) {
            auto& [node, idx] = stk.back();
            auto succs = node->successors_blocks();
            if (idx < succs.size()) {
                auto* child = succs[idx++];
                if (child && !visited.contains(child)) {
                    visited.insert(child);
                    stk.push_back({child, 0});
                }
            } else {
                post_order.push_back(node);
                stk.pop_back();
            }
        }

        rpo_.assign(post_order.rbegin(), post_order.rend());
        for (std::size_t i = 0; i < rpo_.size(); ++i) {
            rpo_index_[rpo_[i]] = i;
        }

        // 2. Initialize idom: entry dominates itself.
        idom_[entry_] = entry_;

        auto intersect = [&](TransitionBlock* b1, TransitionBlock* b2) -> TransitionBlock* {
            while (b1 != b2) {
                while (rpo_index_.at(b1) > rpo_index_.at(b2)) b1 = idom_.at(b1);
                while (rpo_index_.at(b2) > rpo_index_.at(b1)) b2 = idom_.at(b2);
            }
            return b1;
        };

        // 3. Iterate until fixed point.
        bool changed = true;
        while (changed) {
            changed = false;
            for (TransitionBlock* b : rpo_) {
                if (b == entry_) continue;

                TransitionBlock* new_idom = nullptr;
                for (TransitionBlock* pred : b->predecessors_blocks()) {
                    if (!pred || !idom_.contains(pred)) continue;
                    if (!new_idom) {
                        new_idom = pred;
                    } else {
                        new_idom = intersect(new_idom, pred);
                    }
                }

                if (new_idom && idom_[b] != new_idom) {
                    idom_[b] = new_idom;
                    changed = true;
                }
            }
        }

        domtree_valid_ = true;
    }

    /// Query dominance: does `a` dominate `b`?
    /// Uses the cached dominator tree for O(depth) per query instead of O(V+E).
    bool dominates(TransitionBlock* a, TransitionBlock* b) const {
        if (a == b) return true;
        if (a == entry_) return true;
        if (b == entry_) return false;
        if (!a || !b) return false;
        
        if (!domtree_valid_) {
            compute_domtree();
        }

        // Walk the idom chain from b up to entry; if we encounter a, it dominates.
        TransitionBlock* cur = b;
        while (cur != entry_ && cur != nullptr) {
            auto it = idom_.find(cur);
            if (it == idom_.end()) return false; // b is unreachable from entry
            if (it->second == a) return true;
            if (it->second == cur) return false; // self-loop in idom = entry
            cur = it->second;
        }
        return cur == a;
    }

    /// Invalidate the cached dominator tree. Must be called after structural
    /// modifications (add/remove block or edge).
    void invalidate_domtree() const { domtree_valid_ = false; }

private:
    DecompilerArena& arena_;
    TransitionBlock* entry_ = nullptr;
    std::vector<TransitionBlock*> blocks_;

    // Cached dominator tree (computed lazily on first dominates() query).
    mutable std::unordered_map<TransitionBlock*, TransitionBlock*> idom_;
    mutable std::vector<TransitionBlock*> rpo_;
    mutable std::unordered_map<TransitionBlock*, std::size_t> rpo_index_;
    mutable bool domtree_valid_ = false;
};

} // namespace aletheia
