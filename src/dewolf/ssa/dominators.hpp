#pragma once
#include "../structures/cfg.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace dewolf {

class DominatorTree {
public:
    explicit DominatorTree(const ControlFlowGraph& cfg, bool post_dominators = false);

    BasicBlock* idom(BasicBlock* node) const;
    const std::vector<BasicBlock*>& dominance_frontier(BasicBlock* node) const;
    
    bool strictly_dominates(BasicBlock* a, BasicBlock* b) const;
    bool dominates(BasicBlock* a, BasicBlock* b) const;

private:
    void compute_dominators(const ControlFlowGraph& cfg, bool post_dominators);
    void compute_dominance_frontiers(const ControlFlowGraph& cfg, bool post_dominators);
    BasicBlock* intersect(BasicBlock* b1, BasicBlock* b2, const std::unordered_map<BasicBlock*, std::size_t>& post_order_idx) const;

    std::unordered_map<BasicBlock*, BasicBlock*> idoms_;
    std::unordered_map<BasicBlock*, std::vector<BasicBlock*>> frontiers_;
};

} // namespace dewolf
