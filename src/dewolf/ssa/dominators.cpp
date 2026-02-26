#include "dominators.hpp"
#include <algorithm>

namespace dewolf {

DominatorTree::DominatorTree(const ControlFlowGraph& cfg) {
    if (!cfg.entry_block()) return;
    compute_dominators(cfg);
    compute_dominance_frontiers(cfg);
}

BasicBlock* DominatorTree::idom(BasicBlock* node) const {
    auto it = idoms_.find(node);
    if (it != idoms_.end()) return it->second;
    return nullptr;
}

const std::vector<BasicBlock*>& DominatorTree::dominance_frontier(BasicBlock* node) const {
    static const std::vector<BasicBlock*> empty;
    auto it = frontiers_.find(node);
    if (it != frontiers_.end()) return it->second;
    return empty;
}

bool DominatorTree::strictly_dominates(BasicBlock* a, BasicBlock* b) const {
    if (a == b) return false;
    BasicBlock* current = idom(b);
    while (current != nullptr) {
        if (current == a) return true;
        current = idom(current);
    }
    return false;
}

bool DominatorTree::dominates(BasicBlock* a, BasicBlock* b) const {
    if (a == b) return true;
    return strictly_dominates(a, b);
}

BasicBlock* DominatorTree::intersect(BasicBlock* b1, BasicBlock* b2, const std::unordered_map<BasicBlock*, std::size_t>& post_order_idx) const {
    BasicBlock* finger1 = b1;
    BasicBlock* finger2 = b2;

    while (finger1 != finger2) {
        while (post_order_idx.at(finger1) < post_order_idx.at(finger2)) {
            finger1 = idoms_.at(finger1);
        }
        while (post_order_idx.at(finger2) < post_order_idx.at(finger1)) {
            finger2 = idoms_.at(finger2);
        }
    }
    return finger1;
}

void DominatorTree::compute_dominators(const ControlFlowGraph& cfg) {
    auto post_order = cfg.post_order();
    if (post_order.empty()) return;

    std::unordered_map<BasicBlock*, std::size_t> post_order_idx;
    for (std::size_t i = 0; i < post_order.size(); ++i) {
        post_order_idx[post_order[i]] = i;
    }

    BasicBlock* start_node = cfg.entry_block();
    idoms_[start_node] = start_node;

    auto rpo = post_order;
    std::reverse(rpo.begin(), rpo.end());

    bool changed = true;
    while (changed) {
        changed = false;

        for (BasicBlock* b : rpo) {
            if (b == start_node) continue;

            BasicBlock* new_idom = nullptr;
            for (Edge* edge : b->predecessors()) {
                BasicBlock* p = edge->source();
                if (idoms_.contains(p)) {
                    if (new_idom == nullptr) {
                        new_idom = p;
                    } else {
                        new_idom = intersect(p, new_idom, post_order_idx);
                    }
                }
            }

            if (new_idom != nullptr && (!idoms_.contains(b) || idoms_[b] != new_idom)) {
                idoms_[b] = new_idom;
                changed = true;
            }
        }
    }

    // Fix up entry block idom to be nullptr for consistency with strict dominance
    idoms_[start_node] = nullptr;
}

void DominatorTree::compute_dominance_frontiers(const ControlFlowGraph& cfg) {
    for (BasicBlock* b : cfg.blocks()) {
        if (b->predecessors().size() >= 2) {
            for (Edge* edge : b->predecessors()) {
                BasicBlock* runner = edge->source();
                while (runner != nullptr && runner != idom(b)) {
                    // check if b is already in the frontier
                    auto& f = frontiers_[runner];
                    if (std::find(f.begin(), f.end(), b) == f.end()) {
                        f.push_back(b);
                    }
                    runner = idom(runner);
                }
            }
        }
    }
}

} // namespace dewolf
