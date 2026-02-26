#include "structurer.hpp"
#include "../ssa/dominators.hpp"
#include "../../dewolf_logic/z3_logic.hpp"
#include "graph_slice/graph_slice.hpp"
#include "reaching_conditions/reaching_conditions.hpp"
#include "cbr/cbr.hpp"
#include "car/car.hpp"
#include <unordered_set>
#include <string>

namespace dewolf {

void CyclicRegionFinder::process(TransitionCFG& cfg) {
    if (!cfg.entry()) return;
    
    std::unordered_set<TransitionBlock*> visited;
    std::unordered_set<TransitionBlock*> path;
    std::vector<std::pair<TransitionBlock*, TransitionBlock*>> back_edges;

    auto dfs = [&](TransitionBlock* node, auto& dfs_ref) -> void {
        if (!node) return;
        visited.insert(node);
        path.insert(node);

        for (auto* succ : node->successors()) {
            if (path.contains(succ)) {
                back_edges.push_back({node, succ});
            } else if (!visited.contains(succ)) {
                dfs_ref(succ, dfs_ref);
            }
        }
        path.erase(node);
    };

    dfs(cfg.entry(), dfs);

    for (auto& edge : back_edges) {
        TransitionBlock* tail = edge.first;
        TransitionBlock* header = edge.second;

        SeqNode* body = arena_.create<SeqNode>();
        body->add_node(header->ast_node());
        if (tail != header) {
            body->add_node(tail->ast_node());
        }

        LoopNode* loop = arena_.create<LoopNode>(body);
        header->set_ast_node(loop);
    }
}

void AcyclicRegionRestructurer::process(TransitionCFG& cfg) {
    if (!cfg.entry()) return;

    std::unordered_set<TransitionBlock*> all_nodes;
    for (auto* b : cfg.blocks()) {
        all_nodes.insert(b);
    }

    restructure_region(&cfg, cfg.entry(), all_nodes);
}

void AcyclicRegionRestructurer::restructure_region(TransitionCFG* t_cfg, TransitionBlock* header, const std::unordered_set<TransitionBlock*>& region) {
    TransitionCFG* slice = GraphSlice::compute_graph_slice_for_region(arena_, t_cfg, header, region, false);

    z3::context ctx;
    auto reaching_conditions = ReachingConditions::compute(ctx, slice, header, t_cfg);

    SeqNode* seq = arena_.create<SeqNode>();
    for (TransitionBlock* block : slice->blocks()) {
        seq->add_node(block->ast_node());
    }

    // 4. Condition-Based Refinement (CBR) - De Morgan's Law Application & Branch Synthesis
    AstNode* cbr_root = ConditionBasedRefinement::refine(arena_, ctx, seq, reaching_conditions);

    // 5. Condition-Aware Refinement (CAR) - Switch Statement Extraction
    AstNode* car_root = ConditionAwareRefinement::refine(arena_, ctx, cbr_root, reaching_conditions);

    header->set_ast_node(car_root);
}

} // namespace dewolf
