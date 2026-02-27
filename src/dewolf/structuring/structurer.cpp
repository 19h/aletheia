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

    cfg.refresh_edge_properties();
    auto loop_heads = cfg.get_loop_heads();

    while (!loop_heads.empty()) {
        TransitionBlock* head = loop_heads.back();
        loop_heads.pop_back();

        size_t initial_nodes = cfg.blocks().size();

        // 1. Compute initial loop nodes (GraphSlice from head to latching nodes)
        std::vector<TransitionBlock*> latching_nodes;
        for (auto* e : head->predecessors()) {
            if (e->property() == EdgeProperty::Back || e->property() == EdgeProperty::Retreating) {
                latching_nodes.push_back(e->source());
            }
        }
        
        TransitionCFG* loop_region = GraphSlice::compute_graph_slice_for_sink_nodes(arena_, &cfg, head, latching_nodes, false);
        std::unordered_set<TransitionBlock*> region_set(loop_region->blocks().begin(), loop_region->blocks().end());

        // 2. Restructure Abnormal Entry (TODO)
        // If there's a retreating edge into head
        bool has_retreating = false;
        for (auto* e : head->predecessors()) {
            if (e->property() == EdgeProperty::Retreating) has_retreating = true;
        }
        if (has_retreating) {
            // restructure abnormal entry
            // ... (implement H.10.1) ...
        }

        // 3. Compute Loop Successors
        std::vector<TransitionBlock*> loop_successors;
        std::unordered_set<TransitionBlock*> succ_set;
        for (auto* node : loop_region->blocks()) {
            for (auto* succ : node->successors_blocks()) {
                if (!region_set.contains(succ) && !succ_set.contains(succ)) {
                    loop_successors.push_back(succ);
                    succ_set.insert(succ);
                }
            }
        }

        // 4. Restructure Abnormal Exit (TODO)
        if (loop_successors.size() > 1) {
            // restructure abnormal exit
            // ... (implement H.10.2) ...
        }

        // 5. Restructure acyclic loop body
        auto* loop_cfg = arena_.create<TransitionCFG>(arena_);
        std::unordered_map<TransitionBlock*, TransitionBlock*> block_map;
        for (auto* node : loop_region->blocks()) {
            auto* clone = arena_.create<TransitionBlock>(node->ast_node());
            block_map[node] = clone;
            loop_cfg->add_block(clone);
        }
        loop_cfg->set_entry(block_map[head]);

        for (auto* node : loop_region->blocks()) {
            auto* clone_src = block_map[node];
            for (auto* e : node->successors()) {
                auto* succ = e->sink();
                if (succ == head && node != head && region_set.contains(node)) {
                    // Back edge -> Continue node
                    auto* cont_block = arena_.create<BasicBlock>(9999);
                    cont_block->add_instruction(arena_.create<ContinueInstr>());
                    auto* cont_trans = arena_.create<TransitionBlock>(arena_.create<CodeNode>(cont_block));
                    loop_cfg->add_block(cont_trans);
                    loop_cfg->add_edge(clone_src, cont_trans, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)), EdgeProperty::NonLoop);
                } else if (!region_set.contains(succ)) {
                    // Exit edge -> Break node
                    auto* brk_block = arena_.create<BasicBlock>(9998);
                    brk_block->add_instruction(arena_.create<BreakInstr>());
                    auto* brk_trans = arena_.create<TransitionBlock>(arena_.create<CodeNode>(brk_block));
                    loop_cfg->add_block(brk_trans);
                    loop_cfg->add_edge(clone_src, brk_trans, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)), EdgeProperty::NonLoop);
                } else if (block_map.contains(succ)) {
                    auto* clone_dst = block_map[succ];
                    loop_cfg->add_edge(clone_src, clone_dst, e->tag(), e->property());
                }
            }
        }

        AcyclicRegionRestructurer acyclic(task_);
        acyclic.process(*loop_cfg);

        AstNode* loop_body_ast = loop_cfg->entry()->ast_node();
        auto* endless_loop = arena_.create<WhileLoopNode>(loop_body_ast);
        AstNode* refined = LoopStructurer::refine_loop(arena_, endless_loop);
        head->set_ast_node(refined);

        for (auto* succ : loop_successors) {
            for (auto* node : loop_region->blocks()) {
                if (node != head) {
                    cfg.remove_edge_between(node, succ);
                }
            }
            bool linked = false;
            for (auto* s : head->successors_blocks()) if (s == succ) { linked = true; break; }
            if (!linked) cfg.add_edge(head, succ, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)));
        }

        for (auto* node : loop_region->blocks()) {
            if (node == head) continue;
            // Need to remove edges properly
            std::vector<TransitionEdge*> out_edges = node->successors();
            for (auto* e : out_edges) cfg.remove_edge(e);
            std::vector<TransitionEdge*> in_edges = node->predecessors();
            for (auto* e : in_edges) cfg.remove_edge(e);
            cfg.remove_block(node);
        }
        cfg.remove_edge_between(head, head); // remove self loop

        if (cfg.blocks().size() != initial_nodes) {
            cfg.refresh_edge_properties();
            loop_heads = cfg.get_loop_heads();
        }
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
                    for (auto* p : curr->predecessors_blocks()) {
                        if (!region.contains(p)) {
                            all_preds_in = false;
                            break;
                        }
                    }
                }
                
                if (all_preds_in) {
                    region.insert(curr);
                    for (auto* s : curr->successors_blocks()) {
                        stack.push_back(s);
                    }
                }
            }
            
            if (region.size() <= 1) continue;

            std::unordered_set<TransitionBlock*> exits;
            for (auto* node : region) {
                for (auto* s : node->successors_blocks()) {
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
            for (auto* p : best_header->predecessors_blocks()) {
                if (!best_region.contains(p)) incoming_preds.push_back(p);
            }
            for (auto* p : incoming_preds) {
                cfg.remove_edge_between(p, best_header);
                cfg.add_edge(p, collapsed_block, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)));
            }
            
            if (best_exit) {
                cfg.add_edge(collapsed_block, best_exit, dewolf_logic::LogicCondition(task_.z3_ctx().bool_val(true)));
                
                for (auto* node : best_region) {
                    cfg.remove_edge_between(node, best_exit);
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

    z3::context& ctx = task_.z3_ctx();
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
