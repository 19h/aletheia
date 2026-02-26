#include "structurer.hpp"
#include "../ssa/dominators.hpp"
#include "../../dewolf_logic/z3_logic.hpp"
#include "graph_slice/graph_slice.hpp"
#include "reaching_conditions/reaching_conditions.hpp"
#include "cbr/cbr.hpp"
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

    // Simulate acyclic region extraction
    // In Python DeWolf: acyclic_region_finder.find(node)
    std::unordered_set<TransitionBlock*> all_nodes;
    for (auto* b : cfg.blocks()) {
        all_nodes.insert(b);
    }

    // Call the exact structuring method
    restructure_region(&cfg, cfg.entry(), all_nodes);
}

void AcyclicRegionRestructurer::restructure_region(TransitionCFG* t_cfg, TransitionBlock* header, const std::unordered_set<TransitionBlock*>& region) {
    // 1. Compute graph slice
    TransitionCFG* slice = GraphSlice::compute_graph_slice_for_region(arena_, t_cfg, header, region, false);

    // 2. Compute Reaching Conditions
    z3::context ctx;
    auto reaching_conditions = ReachingConditions::compute(ctx, slice, header, t_cfg);

    // 3. Construct Initial AST
    // Translate the slice into a SeqNode where each slice node is mapped to a CodeNode.
    SeqNode* seq = arena_.create<SeqNode>();
    for (TransitionBlock* block : slice->blocks()) {
        seq->add_node(block->ast_node());
    }

    // 4. Apply Condition-Based Refinement (CBR)
    // Passes the flat sequence and the mathematical reaching conditions to the Refiner,
    // which recursively collapses complementary condition expressions into nested IfNodes/SwitchNodes.
    AstNode* structured_root = ConditionBasedRefinement::refine(arena_, ctx, seq, reaching_conditions);

    // Update the transition CFG entry with the final refined AST structure
    header->set_ast_node(structured_root);
}

} // namespace dewolf
