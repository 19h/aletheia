#pragma once

#include "transition_cfg.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dewolf {

class ReachabilityGraph {
public:
    explicit ReachabilityGraph(TransitionCFG* cfg) {
        if (!cfg) return;
        for (TransitionBlock* block : cfg->blocks()) {
            reachable_[block] = compute_reachable_from(block);
        }
    }

    bool reaches(TransitionBlock* from, TransitionBlock* to) const {
        auto it = reachable_.find(from);
        if (it == reachable_.end()) return false;
        return it->second.contains(to);
    }

private:
    std::unordered_set<TransitionBlock*> compute_reachable_from(TransitionBlock* start) {
        std::unordered_set<TransitionBlock*> visited;
        if (!start) return visited;

        std::vector<TransitionBlock*> stack = {start};
        while (!stack.empty()) {
            TransitionBlock* cur = stack.back();
            stack.pop_back();
            if (!cur || visited.contains(cur)) continue;
            visited.insert(cur);
            for (TransitionBlock* succ : cur->successors_blocks()) {
                if (!visited.contains(succ)) {
                    stack.push_back(succ);
                }
            }
        }
        return visited;
    }

    std::unordered_map<TransitionBlock*, std::unordered_set<TransitionBlock*>> reachable_;
};

class SiblingReachability {
public:
    explicit SiblingReachability(const ReachabilityGraph& graph) : graph_(graph) {}

    std::vector<TransitionBlock*> order_blocks(const std::vector<TransitionBlock*>& blocks) const {
        std::unordered_map<TransitionBlock*, std::size_t> index;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            index[blocks[i]] = i;
        }

        std::unordered_map<TransitionBlock*, std::vector<TransitionBlock*>> edges;
        std::unordered_map<TransitionBlock*, int> indegree;
        for (TransitionBlock* b : blocks) indegree[b] = 0;

        for (std::size_t i = 0; i < blocks.size(); ++i) {
            for (std::size_t j = 0; j < blocks.size(); ++j) {
                if (i == j) continue;
                TransitionBlock* a = blocks[i];
                TransitionBlock* b = blocks[j];
                const bool a_to_b = graph_.reaches(a, b);
                const bool b_to_a = graph_.reaches(b, a);
                if (a_to_b && !b_to_a) {
                    edges[a].push_back(b);
                }
            }
        }

        for (auto& [src, dsts] : edges) {
            std::sort(dsts.begin(), dsts.end(), [&](TransitionBlock* lhs, TransitionBlock* rhs) {
                return index[lhs] < index[rhs];
            });
            dsts.erase(std::unique(dsts.begin(), dsts.end()), dsts.end());
            for (TransitionBlock* dst : dsts) indegree[dst]++;
        }

        std::vector<TransitionBlock*> ready;
        for (TransitionBlock* b : blocks) {
            if (indegree[b] == 0) ready.push_back(b);
        }
        std::sort(ready.begin(), ready.end(), [&](TransitionBlock* lhs, TransitionBlock* rhs) {
            return index[lhs] < index[rhs];
        });

        std::vector<TransitionBlock*> ordered;
        while (!ready.empty()) {
            TransitionBlock* cur = ready.front();
            ready.erase(ready.begin());
            ordered.push_back(cur);

            for (TransitionBlock* dst : edges[cur]) {
                indegree[dst]--;
                if (indegree[dst] == 0) {
                    ready.push_back(dst);
                }
            }
            std::sort(ready.begin(), ready.end(), [&](TransitionBlock* lhs, TransitionBlock* rhs) {
                return index[lhs] < index[rhs];
            });
        }

        if (ordered.size() != blocks.size()) {
            std::unordered_set<TransitionBlock*> seen(ordered.begin(), ordered.end());
            for (TransitionBlock* b : blocks) {
                if (!seen.contains(b)) ordered.push_back(b);
            }
        }

        return ordered;
    }

private:
    const ReachabilityGraph& graph_;
};

class CaseDependencyGraph {
public:
    static std::vector<CaseNode*> order_cases(const std::vector<CaseNode*>& cases) {
        std::vector<CaseNode*> ordered = cases;
        std::stable_sort(ordered.begin(), ordered.end(), [](CaseNode* lhs, CaseNode* rhs) {
            if (lhs->is_default() != rhs->is_default()) {
                return !lhs->is_default();
            }
            if (lhs->is_default() && rhs->is_default()) {
                return false;
            }
            return lhs->value() < rhs->value();
        });
        return ordered;
    }
};

} // namespace dewolf
