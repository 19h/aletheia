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

    // Step 1: Detect all back edges via DFS
    std::unordered_set<TransitionBlock*> visited;
    std::unordered_set<TransitionBlock*> on_path;
    // back_edges: (tail, header) where tail->header is a back edge
    std::vector<std::pair<TransitionBlock*, TransitionBlock*>> back_edges;

    auto dfs = [&](TransitionBlock* node, auto& dfs_ref) -> void {
        if (!node) return;
        visited.insert(node);
        on_path.insert(node);

        for (auto* succ : node->successors()) {
            if (on_path.contains(succ)) {
                back_edges.push_back({node, succ});
            } else if (!visited.contains(succ)) {
                dfs_ref(succ, dfs_ref);
            }
        }
        on_path.erase(node);
    };

    dfs(cfg.entry(), dfs);

    // Process each back edge: compute the natural loop region, synthesize
    // break/continue, restructure the loop body, and wrap in a loop AST node.
    for (auto& [tail, header] : back_edges) {
        // --- Step 2: Compute the natural loop region ---
        // The natural loop is the set of all nodes from which the header
        // can be reached by traversing only backwards edges. We use the
        // standard algorithm: start with {header, tail}, then for each
        // node in the worklist that is not the header, add its predecessors.
        std::unordered_set<TransitionBlock*> loop_region;
        loop_region.insert(header);

        std::vector<TransitionBlock*> worklist;
        if (tail != header) {
            loop_region.insert(tail);
            worklist.push_back(tail);
        }

        while (!worklist.empty()) {
            TransitionBlock* n = worklist.back();
            worklist.pop_back();
            for (auto* pred : n->predecessors()) {
                if (!loop_region.contains(pred)) {
                    loop_region.insert(pred);
                    worklist.push_back(pred);
                }
            }
        }

        // --- Step 3: Compute loop successors (exit targets) ---
        std::unordered_set<TransitionBlock*> loop_successors;
        for (auto* node : loop_region) {
            for (auto* succ : node->successors()) {
                if (!loop_region.contains(succ)) {
                    loop_successors.insert(succ);
                }
            }
        }

        // --- Step 4: Build a sub-TransitionCFG for the loop region ---
        // This allows us to run the acyclic restructurer on it.
        auto* loop_cfg = arena_.create<TransitionCFG>();

        // Create break/continue synthesis blocks and copy region blocks
        // into the loop sub-CFG, with back edges and exit edges replaced.
        std::unordered_map<TransitionBlock*, TransitionBlock*> block_map;
        for (auto* node : loop_region) {
            auto* clone = arena_.create<TransitionBlock>(node->ast_node());
            block_map[node] = clone;
            loop_cfg->add_block(clone);
        }
        loop_cfg->set_entry(block_map[header]);

        // For each edge in the original region, wire up the clone graph.
        // Replace back edges (->header) with Continue nodes.
        // Replace exit edges (->successor outside region) with Break nodes.
        for (auto* node : loop_region) {
            auto* clone_src = block_map[node];
            for (auto* succ : node->successors()) {
                if (succ == header && node != header) {
                    // Back edge -> insert Continue node
                    auto* cont_block = arena_.create<BasicBlock>(9999);
                    cont_block->add_instruction(arena_.create<ContinueInstr>());
                    auto* cont_code = arena_.create<CodeNode>(cont_block);
                    auto* cont_trans = arena_.create<TransitionBlock>(cont_code);
                    loop_cfg->add_block(cont_trans);
                    clone_src->add_successor(cont_trans);
                    cont_trans->add_predecessor(clone_src);
                } else if (loop_successors.contains(succ)) {
                    // Exit edge -> insert Break node
                    auto* brk_block = arena_.create<BasicBlock>(9998);
                    brk_block->add_instruction(arena_.create<BreakInstr>());
                    auto* brk_code = arena_.create<CodeNode>(brk_block);
                    auto* brk_trans = arena_.create<TransitionBlock>(brk_code);
                    loop_cfg->add_block(brk_trans);
                    clone_src->add_successor(brk_trans);
                    brk_trans->add_predecessor(clone_src);
                } else if (block_map.contains(succ)) {
                    // Internal edge within the region
                    auto* clone_dst = block_map[succ];
                    clone_src->add_successor(clone_dst);
                    clone_dst->add_predecessor(clone_src);
                }
            }
        }

        // --- Step 5: Restructure the loop body as acyclic ---
        // The loop sub-CFG is now a DAG (no back edges or exit edges).
        AcyclicRegionRestructurer acyclic(arena_);
        acyclic.process(*loop_cfg);

        // The acyclic restructurer collapses everything into the entry
        // block's AST node.
        AstNode* loop_body_ast = loop_cfg->entry()->ast_node();

        // --- Step 6: Wrap in WhileLoopNode and refine ---
        auto* endless_loop = arena_.create<WhileLoopNode>(loop_body_ast);
        AstNode* refined = LoopStructurer::refine_loop(arena_, endless_loop);

        // Replace the header's AST node with the refined loop
        header->set_ast_node(refined);

        // --- Step 7: Remove other region nodes from the main CFG ---
        // Redirect edges: all predecessors of the header from outside the
        // region already point to header. All exit edges from region nodes
        // should now point from header to the successor.
        for (auto* succ : loop_successors) {
            // Remove edges from region nodes to this successor
            for (auto* node : loop_region) {
                if (node != header) {
                    node->remove_successor(succ);
                    succ->remove_predecessor(node);
                }
            }
            // Add edge from header to successor (if not already present)
            bool already_linked = false;
            for (auto* s : header->successors()) {
                if (s == succ) { already_linked = true; break; }
            }
            if (!already_linked) {
                header->add_successor(succ);
                succ->add_predecessor(header);
            }
        }

        // Remove all non-header region nodes from the main CFG
        for (auto* node : loop_region) {
            if (node == header) continue;
            // Remove all edges involving this node
            for (auto* s : node->successors()) {
                s->remove_predecessor(node);
            }
            for (auto* p : node->predecessors()) {
                p->remove_successor(node);
            }
            cfg.remove_block(node);
        }

        // Remove the header's back-edge successors that were back edges
        header->remove_successor(header); // self-loop if it existed
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
