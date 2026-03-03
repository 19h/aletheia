#include "dominators.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>

namespace aletheia {

namespace {

std::uint64_t block_order_key(BasicBlock* block) {
    if (!block) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(block->id());
}

std::vector<BasicBlock*> sorted_blocks_by_id(const std::vector<BasicBlock*>& blocks) {
    std::vector<BasicBlock*> sorted = blocks;
    std::stable_sort(sorted.begin(), sorted.end(), [](BasicBlock* lhs, BasicBlock* rhs) {
        return block_order_key(lhs) < block_order_key(rhs);
    });
    return sorted;
}

std::vector<Edge*> sorted_edges(const std::vector<Edge*>& edges, bool reverse_edges) {
    std::vector<Edge*> sorted = edges;
    std::stable_sort(sorted.begin(), sorted.end(), [reverse_edges](Edge* lhs, Edge* rhs) {
        BasicBlock* lhs_target = lhs ? (reverse_edges ? lhs->source() : lhs->target()) : nullptr;
        BasicBlock* rhs_target = rhs ? (reverse_edges ? rhs->source() : rhs->target()) : nullptr;

        const std::uint64_t lhs_key = block_order_key(lhs_target);
        const std::uint64_t rhs_key = block_order_key(rhs_target);
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key;
        }

        const int lhs_type = lhs ? static_cast<int>(lhs->type()) : std::numeric_limits<int>::max();
        const int rhs_type = rhs ? static_cast<int>(rhs->type()) : std::numeric_limits<int>::max();
        if (lhs_type != rhs_type) {
            return lhs_type < rhs_type;
        }

        const int lhs_kind = lhs ? static_cast<int>(lhs->edge_kind()) : std::numeric_limits<int>::max();
        const int rhs_kind = rhs ? static_cast<int>(rhs->edge_kind()) : std::numeric_limits<int>::max();
        if (lhs_kind != rhs_kind) {
            return lhs_kind < rhs_kind;
        }

        return false;
    });
    return sorted;
}

} // namespace

static void compute_post_order(BasicBlock* block, std::unordered_set<std::size_t>& visited, std::vector<BasicBlock*>& result, bool reverse_edges) {
    if (!block) return;
    
    visited.insert(block->id());
    
    auto& edges = reverse_edges ? block->predecessors() : block->successors();
    for (Edge* edge : sorted_edges(edges, reverse_edges)) {
        if (!edge) {
            continue;
        }
        BasicBlock* target = reverse_edges ? edge->source() : edge->target();
        if (target && visited.find(target->id()) == visited.end()) {
            compute_post_order(target, visited, result, reverse_edges);
        }
    }
    
    result.push_back(block);
}

DominatorTree::DominatorTree(const ControlFlowGraph& cfg, bool post_dominators) {
    if (!cfg.entry_block()) return;
    compute_dominators(cfg, post_dominators);
    compute_dominance_frontiers(cfg, post_dominators);
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
        if (!finger1 || !finger2) break;
        while (finger1 && finger2 && post_order_idx.at(finger1) < post_order_idx.at(finger2)) {
            auto it = idoms_.find(finger1);
            if (it == idoms_.end()) break;
            finger1 = it->second;
        }
        while (finger1 && finger2 && post_order_idx.at(finger2) < post_order_idx.at(finger1)) {
            auto it = idoms_.find(finger2);
            if (it == idoms_.end()) break;
            finger2 = it->second;
        }
    }
    return finger1;
}

void DominatorTree::compute_dominators(const ControlFlowGraph& cfg, bool post_dominators) {
    std::vector<BasicBlock*> post_order;
    std::unordered_set<std::size_t> visited;
    const std::vector<BasicBlock*> ordered_blocks = sorted_blocks_by_id(cfg.blocks());
    
    // For post-dominators, we need a virtual exit node, or we collect all nodes with no successors
    std::vector<BasicBlock*> start_nodes;
    if (post_dominators) {
        for (BasicBlock* b : ordered_blocks) {
            if (b->successors().empty()) {
                start_nodes.push_back(b);
            }
        }
        // If there's a single exit, we use it. If multiple, we should ideally use a virtual exit.
        // For simplicity, we just use the first exit block.
        if (!start_nodes.empty()) {
            compute_post_order(start_nodes[0], visited, post_order, true);
        }
    } else {
        start_nodes.push_back(cfg.entry_block());
        compute_post_order(start_nodes[0], visited, post_order, false);
    }
    
    for (BasicBlock* block : ordered_blocks) {
        if (visited.find(block->id()) == visited.end()) {
            compute_post_order(block, visited, post_order, post_dominators);
        }
    }

    if (post_order.empty() || start_nodes.empty()) return;

    std::unordered_map<BasicBlock*, std::size_t> post_order_idx;
    for (std::size_t i = 0; i < post_order.size(); ++i) {
        post_order_idx[post_order[i]] = i;
    }

    BasicBlock* start_node = start_nodes[0];
    idoms_[start_node] = start_node;

    auto rpo = post_order;
    std::reverse(rpo.begin(), rpo.end());

    bool changed = true;
    while (changed) {
        changed = false;

        for (BasicBlock* b : rpo) {
            if (b == start_node) continue;

            BasicBlock* new_idom = nullptr;
            auto& edges = post_dominators ? b->successors() : b->predecessors();
            
            for (Edge* edge : sorted_edges(edges, post_dominators)) {
                if (!edge) {
                    continue;
                }
                BasicBlock* p = post_dominators ? edge->target() : edge->source();
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

    idoms_[start_node] = nullptr;

    // Populate children_
    for (BasicBlock* node : ordered_blocks) {
        auto it = idoms_.find(node);
        if (it != idoms_.end() && it->second) {
            children_[it->second].push_back(node);
        }
    }
    for (auto& [parent, kids] : children_) {
        std::stable_sort(kids.begin(), kids.end(), [](BasicBlock* lhs, BasicBlock* rhs) {
            return block_order_key(lhs) < block_order_key(rhs);
        });
    }
}

const std::vector<BasicBlock*>& DominatorTree::children(BasicBlock* node) const {
    static const std::vector<BasicBlock*> empty;
    auto it = children_.find(node);
    if (it != children_.end()) return it->second;
    return empty;
}

void DominatorTree::compute_dominance_frontiers(const ControlFlowGraph& cfg, bool post_dominators) {
    for (BasicBlock* b : sorted_blocks_by_id(cfg.blocks())) {
        auto& edges = post_dominators ? b->successors() : b->predecessors();
        if (edges.size() >= 2) {
            for (Edge* edge : sorted_edges(edges, post_dominators)) {
                if (!edge) {
                    continue;
                }
                BasicBlock* runner = post_dominators ? edge->target() : edge->source();
                while (runner != nullptr && runner != idom(b)) {
                    auto& f = frontiers_[runner];
                    if (std::find(f.begin(), f.end(), b) == f.end()) {
                        f.push_back(b);
                    }
                    runner = idom(runner);
                }
            }
        }
    }

    for (auto& [block, frontier] : frontiers_) {
        std::stable_sort(frontier.begin(), frontier.end(), [](BasicBlock* lhs, BasicBlock* rhs) {
            return block_order_key(lhs) < block_order_key(rhs);
        });
    }
}

} // namespace aletheia
