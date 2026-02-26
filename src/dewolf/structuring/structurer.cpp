#include "structurer.hpp"
#include "loop_structurer.hpp"
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

        WhileLoopNode* loop = arena_.create<WhileLoopNode>(body);

        // Refine the endless loop into the correct loop type
        AstNode* refined = LoopStructurer::refine_loop(arena_, loop);
        header->set_ast_node(refined);
    }
}

void AcyclicRegionRestructurer::process(TransitionCFG& cfg) {
    if (!cfg.entry()) return;

    bool reduced = true;
    while (reduced && cfg.blocks().size() > 1) {
        reduced = false;

        std::unordered_set<TransitionBlock*> best_region;
        TransitionBlock* best_header = nullptr;
        TransitionBlock* best_exit = nullptr;

        for (TransitionBlock* header : cfg.blocks()) {
            std::unordered_set<TransitionBlock*> region;
            std::unordered_set<TransitionBlock*> visited;
            std::vector<TransitionBlock*> stack = {header};
            
            while (!stack.empty()) {
                TransitionBlock* curr = stack.back();
                stack.pop_back();
                
                if (visited.contains(curr)) continue;
                visited.insert(curr);
                
                bool all_preds_in = true;
                if (curr != header) {
                    for (auto* p : curr->predecessors()) {
                        if (!region.contains(p)) {
                            all_preds_in = false;
                            break;
                        }
                    }
                }
                
                if (all_preds_in) {
                    region.insert(curr);
                    for (auto* s : curr->successors()) {
                        stack.push_back(s);
                    }
                }
            }
            
            if (region.size() <= 1) continue;

            std::unordered_set<TransitionBlock*> exits;
            for (auto* node : region) {
                for (auto* s : node->successors()) {
                    if (!region.contains(s)) {
                        exits.insert(s);
                    }
                }
            }
            
            if (exits.size() <= 1) {
                if (best_region.empty() || region.size() < best_region.size()) {
                    best_region = region;
                    best_header = header;
                    best_exit = exits.empty() ? nullptr : *exits.begin();
                }
            }
        }

        if (!best_region.empty()) {
            restructure_region(&cfg, best_header, best_region);
            
            TransitionBlock* collapsed_block = arena_.create<TransitionBlock>(best_header->ast_node());
            
            std::vector<TransitionBlock*> incoming_preds;
            for (auto* p : best_header->predecessors()) {
                if (!best_region.contains(p)) incoming_preds.push_back(p);
            }
            for (auto* p : incoming_preds) {
                p->remove_successor(best_header);
                p->add_successor(collapsed_block);
                collapsed_block->add_predecessor(p);
            }
            
            if (best_exit) {
                collapsed_block->add_successor(best_exit);
                best_exit->add_predecessor(collapsed_block);
                
                for (auto* node : best_region) {
                    node->remove_successor(best_exit);
                    best_exit->remove_predecessor(node);
                }
            }
            
            for (auto* node : best_region) {
                cfg.remove_block(node);
            }
            cfg.add_block(collapsed_block);
            
            if (cfg.entry() == best_header) {
                cfg.set_entry(collapsed_block);
            }

            reduced = true;
        } else {
            // Force collapse everything remaining into a single sequential block to avoid truncation when there are unstructured jumps
            // But we must call restructure_region on this final set to process Conditions correctly!
            
            std::unordered_set<TransitionBlock*> all_blocks;
            for (auto* b : cfg.blocks()) all_blocks.insert(b);
            
            restructure_region(&cfg, cfg.entry(), all_blocks);
            
            // At this point, the root of cfg.entry() has the fully nested AST!
            break;
        }
    }
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
