#pragma once
#include "../structures/cfg.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace dewolf {

class DominatorTree {
public:
    explicit DominatorTree(const ControlFlowGraph& cfg);

    BasicBlock* idom(BasicBlock* node) const;
    const std::vector<BasicBlock*>& dominance_frontier(BasicBlock* node) const;
    
    // Returns true if A strictly dominates B
    bool strictly_dominates(BasicBlock* a, BasicBlock* b) const;
    // Returns true if A dominates B (or A == B)
    bool dominates(BasicBlock* a, BasicBlock* b) const;

private:
    void compute_dominators(const ControlFlowGraph& cfg);
    void compute_dominance_frontiers(const ControlFlowGraph& cfg);
    BasicBlock* intersect(BasicBlock* b1, BasicBlock* b2, const std::unordered_map<BasicBlock*, std::size_t>& post_order_idx) const;

    std::unordered_map<BasicBlock*, BasicBlock*> idoms_;
    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> frontiers_;
};

} // namespace dewolf
